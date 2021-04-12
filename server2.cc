// Copyright 2017 The Dawn Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "protocol.hh"

#include "dawn/examples/SampleUtils.h"

#include "utils/BackendBinding.h"
#include "utils/GLFWUtils.h"

#include "utils/SystemUtils.h"
#include "utils/WGPUHelpers.h"
#include "GLFW/glfw3.h"

#include <dawn/webgpu_cpp.h>

#include <dawn/dawn_proc.h>
#include <dawn/dawn_wsi.h>
#include <dawn_wire/WireClient.h>
#include <dawn_wire/WireServer.h>
#include <dawn_native/DawnNative.h>

#include <iostream>

#include <unistd.h> // pipe
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h> // F_GETFL, O_NONBLOCK etc

#define DLOG_PREFIX "\e[1;34m[server2]\e[0m "

#ifdef DEBUG
  #define dlog(format, ...) ({ \
    fprintf(stderr, DLOG_PREFIX format " \e[2m(%s %d)\e[0m\n", \
      ##__VA_ARGS__, __FUNCTION__, __LINE__); \
    fflush(stderr); \
  })
  #define errlog(format, ...) \
    (({ fprintf(stderr, "E " format " (%s:%d)\n", ##__VA_ARGS__, __FILE__, __LINE__); \
        fflush(stderr); }))
#else
  #define dlog(...) do{}while(0)
  #define errlog(format, ...) \
(({ fprintf(stderr, "E " format "\n", ##__VA_ARGS__); fflush(stderr); }))
#endif


// Integer align2<T>(Integer n, Integer w)
// rounds up n to closest boundary w (w must be a power of two)
//
// E.g.
//   align2(0, 4) => 0
//   align2(1, 4) => 4
//   align2(2, 4) => 4
//   align2(3, 4) => 4
//   align2(4, 4) => 4
//   align2(5, 4) => 8
//   ...
//
#define align2(n,w) ({ \
  assert(((w) & ((w) - 1)) == 0); /* alignment w is not a power of two */ \
  ((n) + ((w) - 1)) & ~((w) - 1); \
})


static bool FDSetNonBlock(int fd) {
  #ifdef _WIN32
    unsigned long arg = 1;
    ioctlsocket(_get_osfhandle(fd), FIONBIO, &arg); // from libev/ev.c
  #else
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 ||
        fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
        fcntl(fd, F_SETFD, FD_CLOEXEC)) // FD_CLOEXEC for fork
    {
      errno = EWOULDBLOCK;
      return false;
    }
  #endif
  return true;
}

int createUNIXSocket(const char* filename, sockaddr_un* addr) {
  addr->sun_family = AF_UNIX;
  auto filenameLen = strlen(filename);
  if (filenameLen > sizeof(addr->sun_path)-1) {
    errno = ENAMETOOLONG;
    return -1;
  }
  memcpy(addr->sun_path, filename, filenameLen+1);
  return socket(AF_UNIX, SOCK_STREAM, 0);
}

int createUNIXSocketServer(const char* filename) {
  /*struct*/ sockaddr_un addr;
  int fd = createUNIXSocket(filename, &addr);
  if (fd > -1) {
    unlink(filename);
    int acceptQueueSize = 5;
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == -1 ||
        listen(fd, acceptQueueSize) == -1)
    {
      int e = errno;
      close(fd);
      unlink(filename);
      errno = e;
      fd = -1;
    }
  }
  return fd;
}

int connectUNIXSocket(const char* filename) {
  /*struct*/ sockaddr_un addr;
  int fd = createUNIXSocket(filename, &addr);
  if (fd > -1) {
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
      int e = errno;
      close(fd);
      errno = e;
      fd = -1;
    }
  }
  return fd;
}


// backendType
// Default to D3D12, Metal, Vulkan, OpenGL in that order as D3D12 and Metal are the preferred on
// their respective platforms, and Vulkan is preferred to OpenGL
#if defined(DAWN_ENABLE_BACKEND_D3D12)
static wgpu::BackendType backendType = wgpu::BackendType::D3D12;
#elif defined(DAWN_ENABLE_BACKEND_METAL)
static wgpu::BackendType backendType = wgpu::BackendType::Metal;
#elif defined(DAWN_ENABLE_BACKEND_VULKAN)
static wgpu::BackendType backendType = wgpu::BackendType::Vulkan;
#elif defined(DAWN_ENABLE_BACKEND_OPENGL)
static wgpu::BackendType backendType = wgpu::BackendType::OpenGL;
#else
#  error
#endif


static dawn_wire::WireServer* wireServer = nullptr;


// Conn is a connection to a client
struct Conn {
  uint32_t id;

  DawnClientServerProtocol proto;

  Conn(uint32_t id_) : id(id_) {
    proto.onDawnBuffer = [](const char* data, size_t len) {
      dlog("onDawnBuffer len=%zu", len);
      assert(data != nullptr);
      assert(wireServer != nullptr);
      if (wireServer->HandleCommands(data, len) == nullptr)
        dlog("wireServer->HandleCommands FAILED");
    };
  }

  void close() {
    proto.stop();
    if (proto.fd() != -1)
      ::close(proto.fd());
  }
};


#define COMMAND_BUFFER_SIZE 4096*32

const char* sockfile = "server.sock";
Conn* conn0 = nullptr;


class LolCommandBuffer : public dawn_wire::CommandSerializer {
  //dawn_wire::CommandHandler* mHandler = nullptr;
  size_t                     mOffset = 0;
  char                       mBuffer[COMMAND_BUFFER_SIZE];
  const char*                mName = "";
public:
  int w = -1; // file descriptor to write to

  LolCommandBuffer(const char* name) : mName(name) {}
  //LolCommandBuffer(dawn_wire::CommandHandler* handler) : mHandler(handler) {}
  //void SetHandler(dawn_wire::CommandHandler* handler) { mHandler = handler; }

  size_t GetMaximumAllocationSize() const override {
    return sizeof(mBuffer);
  }

  void* GetCmdSpace(size_t size) override {
    assert(size <= sizeof(mBuffer));
    char* result = &mBuffer[mOffset];
    if (sizeof(mBuffer) - size < mOffset) {
      if (!Flush())
        return nullptr;
      return GetCmdSpace(size);
    }
    mOffset += size;
    return result;
  }

  bool Flush() override {
    if (mOffset > 0) {
      dlog("cmd buffer %s Flush writing %zu bytes", mName, mOffset);
      // bool success = mHandler->HandleCommands(mBuffer, mOffset) != nullptr;
      if (!conn0->proto.writeDawnCommands(mBuffer, mOffset))
        return false;
      mOffset = 0;
    }
    return true;
  }
};

static std::unique_ptr<dawn_native::Instance> instance;
static utils::BackendBinding*                 binding = nullptr;
static GLFWwindow*                            window = nullptr;

//static dawn_wire::WireClient* wireClient = nullptr;
static LolCommandBuffer* s2cBuf = nullptr;
//static LolCommandBuffer* c2sBuf = nullptr;

wgpu::Device         device;
wgpu::Queue          queue;
wgpu::SwapChain      swapchain;
wgpu::RenderPipeline pipeline;
// wgpu::TextureView    depthStencilView;
// wgpu::BindGroup      bindGroup;

bool animate = false;


static void PrintDeviceError(WGPUErrorType errorType, const char* message, void*) {
  const char* errorTypeName = "";
  switch (errorType) {
    case WGPUErrorType_Validation:
      errorTypeName = "Validation";
      break;
    case WGPUErrorType_OutOfMemory:
      errorTypeName = "Out of memory";
      break;
    case WGPUErrorType_Unknown:
      errorTypeName = "Unknown";
      break;
    case WGPUErrorType_DeviceLost:
      errorTypeName = "Device lost";
      break;
    default:
      UNREACHABLE();
      return;
  }
  std::cerr << "device error: " << errorTypeName << " error: " << message << std::endl;
}


static void PrintGLFWError(int code, const char* message) {
  std::cerr << "GLFW error: " << code << " - " << message << std::endl;
}


// void flushWireBuffers() {
//   bool s2cSuccess = s2cBuf->Flush();
//   ASSERT(s2cSuccess);
// }


void configureSwapchain(int width, int height);


// onKeyPress is called when keyboard keys are pressed.
//   window   The window that received the event.
//   key      The keyboard key that was pressed or released. (GLFW_KEY_*)
//   scancode The system-specific scancode of the key.
//   action   One of: GLFW_PRESS, GLFW_RELEASE, GLFW_REPEAT
//   mods     Bit field describing which modifier keys were held down. (GLFW_MOD_*)
//
void onKeyPress(GLFWwindow* window, int key, int scancode, int action, int mods) {
  if (action != GLFW_PRESS)
    return;
  printf("key press #%d %s\n", key, glfwGetKeyName(key, scancode));

  switch (key) {
  case GLFW_KEY_A:
    animate = !animate;
    break;

  default:
    break;
  }
}

// onWindowFramebufferResize is called when a window's framebuffer has changed size
//   window The window which framebuffer was resized.
//   width  The new width, in pixels, of the framebuffer.
//   height The new height, in pixels, of the framebuffer.
void onWindowFramebufferResize(GLFWwindow* window, int width, int height) {
  //printf("onWindowFramebufferResize width=%d, height=%d\n", width, height);
  //flushWireBuffers();
  configureSwapchain(width, height);
}

// onWindowResize is called when a window has been resized
//   window The window that was resized.
//   width  The new width, in screen coordinates, of the window.
//   height The new height, in screen coordinates, of the window.
// Note: onWindowResize is called after any call to onWindowFramebufferResize
void onWindowResize(GLFWwindow* window, int width, int height) {
  //printf("onWindowResize width=%d, height=%d\n", width, height);
  // redraw as onWindowFramebufferResize might have replaced the swapchain
  // render_frame();
}

void createOSWindow() {
  assert(window == nullptr);

  glfwSetErrorCallback(PrintGLFWError);
  if (!glfwInit())
    return;

  // Create the test window and discover adapters using it (esp. for OpenGL)
  utils::SetupGLFWWindowHintsForBackend(backendType);
  glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_FALSE);
  window = glfwCreateWindow(640, 480, "hello-wire", /*monitor*/nullptr, nullptr);
  if (!window)
    return;

  // [rsms] move window to bottom right corner of screen
  // glfwSetWindowPos(window, 1920, 960);
  glfwSetWindowPos(window, 2560, 960); // 2nd screen, bottom left corner

  glfwSetKeyCallback(window, onKeyPress);
  glfwSetFramebufferSizeCallback(window, onWindowFramebufferResize);
  glfwSetWindowSizeCallback(window, onWindowResize);
}


const char* backendTypeName(wgpu::BackendType t) {
  switch (t) {
    case wgpu::BackendType::Null:     return "Null";
    case wgpu::BackendType::D3D11:    return "D3D11";
    case wgpu::BackendType::D3D12:    return "D3D12";
    case wgpu::BackendType::Metal:    return "Metal";
    case wgpu::BackendType::Vulkan:   return "Vulkan";
    case wgpu::BackendType::OpenGL:   return "OpenGL";
    case wgpu::BackendType::OpenGLES: return "OpenGLES";
  }
  return "?";
}

const char* adapterTypeName(wgpu::AdapterType t) {
  switch (t) {
    case wgpu::AdapterType::DiscreteGPU:   return "DiscreteGPU";
    case wgpu::AdapterType::IntegratedGPU: return "IntegratedGPU";
    case wgpu::AdapterType::CPU:           return "CPU";
    case wgpu::AdapterType::Unknown:       return "Unknown";
  }
  return "?";
}

// dumpLogAvailableAdapters prints a list of all adapters and their properties
void dumpLogAvailableAdapters(dawn_native::Instance* instance) {
  for (auto&& a : instance->GetAdapters()) {
    wgpu::AdapterProperties p;
    a.GetProperties(&p);
    dlog("adapter %s\n"
      "  description: %s\n"
      "  deviceID:    %u\n"
      "  vendorID:    0x%x\n"
      "  backendType: BackendType::%s\n"
      "  adapterType: AdapterType::%s\n"
      ,
      p.name, p.driverDescription,
      p.deviceID, p.vendorID,
      backendTypeName(p.backendType),
      adapterTypeName(p.adapterType));
  }
}


dawn_native::Adapter backendAdapter;
std::vector<std::pair<uint32_t,uint32_t>> known_devices; // pair of <devId,devGen>


WGPUDevice allocateClientDevice() {
  // See:
  //   https://source.chromium.org/chromium/chromium/src/+/master:gpu/command_buffer/service/webgpu_decoder_impl.cc;drc=8ca5d3a5f1d3e18b363549c0edd4c2494cfb70ea;l=519?q=webgpu_deco
  //   https://source.chromium.org/chromium/chromium/src/+/master:gpu/command_buffer/service/webgpu_decoder_impl.cc;l=843?q=webgpu_deco
  //
  uint32_t devId = 1; // values from calling WireClient.ReserveDevice()
  uint32_t devGen = 0;

  dawn_native::DeviceDescriptor devdescr;
  // devdescr.requiredExtensions.push_back("texture_compression_bc");
  // devdescr.requiredExtensions.push_back("shader_float16");
  // devdescr.requiredExtensions.push_back("pipeline_statistics_query");
  // devdescr.requiredExtensions.push_back("timestamp_query");
  // devdescr.requiredExtensions.push_back("depth_clamping");
  assert(bool(backendAdapter) /* == backendAdapter.mImpl!=nullptr */);
  WGPUDevice device = backendAdapter.CreateDevice(&devdescr);
  if (!device)
    return nullptr;

  assert(wireServer != nullptr);
  if (!wireServer->InjectDevice(device, devId, devGen)) {
    wgpuDeviceRelease(device);
    return nullptr;
  }

  // Device injection takes a ref. The wire now owns the device so release it.
  dawn_native::GetProcs().deviceRelease(device);

  // Save the id and generation of the device. Now, we can query the server for
  // this pair to discover if this device has been destroyed. The list will be
  // checked in PerformPollingWork to tick all the live devices and remove all
  // the dead ones.
  known_devices.emplace_back(devId, devGen);
  return device;
}


wgpu::Device createDawnDevice() {
  if (window == nullptr)
    return wgpu::Device();

  instance = std::make_unique<dawn_native::Instance>();
  utils::DiscoverAdapter(instance.get(), window, backendType);

  dumpLogAvailableAdapters(instance.get());


  // Get an adapter for the backend to use, and create the device.
  //dawn_native::Adapter backendAdapter;
  {
    std::vector<dawn_native::Adapter> adapters = instance->GetAdapters();
    auto adapterIt = std::find_if(adapters.begin(), adapters.end(),
      [](const dawn_native::Adapter adapter) -> bool {
        wgpu::AdapterProperties properties;
        adapter.GetProperties(&properties);
        if (properties.backendType == backendType) {
          dlog("using adapter %s", properties.name);
          return true;
        }
        return false;
      });
    ASSERT(adapterIt != adapters.end());
    backendAdapter = *adapterIt; // global var
  }

  DawnProcTable backendProcs = dawn_native::GetProcs();

  // wire server
  s2cBuf = new LolCommandBuffer("s2c");
  dawn_wire::WireServerDescriptor serverDesc = {};
  serverDesc.procs = &backendProcs;
  serverDesc.serializer = s2cBuf;
  wireServer = new dawn_wire::WireServer(serverDesc);

  // allocate a device for a client
  WGPUDevice backendDevice = allocateClientDevice();
  if (!backendDevice) {
    dlog("allocateClientDevice FAILED");
    return wgpu::Device();
  }
  dlog("allocateClientDevice OK");

  // hook up error reporting
  backendProcs.deviceSetUncapturedErrorCallback(backendDevice, PrintDeviceError, nullptr);

  // setup utils::BackendBinding
  binding = utils::CreateBinding(backendType, window, backendDevice);
  if (binding == nullptr)
    return wgpu::Device();

  dawnProcSetProcs(&backendProcs);

  return wgpu::Device::Acquire(backendDevice);
}


wgpu::TextureFormat GetPreferredSwapChainTextureFormat2() {
  return static_cast<wgpu::TextureFormat>(binding->GetPreferredSwapChainTextureFormat());
}


void configureSwapchain(int width, int height) {
  wgpu::SwapChainDescriptor descriptor;
  descriptor.implementation = binding->GetSwapChainImplementation();
  swapchain = device.CreateSwapChain(nullptr, &descriptor);
  swapchain.Configure(
    GetPreferredSwapChainTextureFormat2(),
    wgpu::TextureUsage::RenderAttachment,
    width,
    height);
}


void init_dawn() {
  device = createDawnDevice();
  queue = device.GetQueue();

  int width = 100;
  int height = 100;
  glfwGetFramebufferSize(window, &width, &height);
  configureSwapchain(width, height);

  const char* vs =
    "[[builtin(vertex_index)]] var<in> VertexIndex : u32;\n"
    "[[builtin(position)]] var<out> Position : vec4<f32>;\n"
    "const pos : array<vec2<f32>, 3> = array<vec2<f32>, 3>(\n"
    "    vec2<f32>( 0.0,  0.5),\n"
    "    vec2<f32>(-0.5, -0.5),\n"
    "    vec2<f32>( 0.5, -0.5)\n"
    ");\n"
    "[[stage(vertex)]] fn main() -> void {\n"
    "    Position = vec4<f32>(pos[VertexIndex], 0.0, 1.0);\n"
    "    return;\n"
    "}\n";
  wgpu::ShaderModule vsModule = utils::CreateShaderModule(device, vs);

  const char* fs =
    "[[location(0)]] var<out> fragColor : vec4<f32>;\n"
    "[[stage(fragment)]] fn main() -> void {\n"
    "    fragColor = vec4<f32>(1.0, 0.0, 0.7, 1.0);\n"
    "    return;\n"
    "}\n";
  wgpu::ShaderModule fsModule = utils::CreateShaderModule(device, fs);

  wgpu::RenderPipelineDescriptor2 descriptor;

  // Fragment state
  wgpu::BlendState blend;
  blend.color.dstFactor = wgpu::BlendFactor::One;
  blend.alpha.dstFactor = wgpu::BlendFactor::One;

  wgpu::ColorTargetState colorTarget;
  colorTarget.format = GetPreferredSwapChainTextureFormat2();
  colorTarget.blend = &blend;

  wgpu::FragmentState fragment;
  fragment.module = fsModule;
  fragment.entryPoint = "main";
  fragment.targetCount = 1;
  fragment.targets = &colorTarget;
  descriptor.fragment = &fragment;

  descriptor.vertex.module = vsModule;
  descriptor.vertex.entryPoint = "main";
  descriptor.vertex.bufferCount = 0;
  descriptor.vertex.buffers = nullptr;

  descriptor.multisample.count = 1;
  descriptor.multisample.mask = 0xFFFFFFFF;
  descriptor.multisample.alphaToCoverageEnabled = false;

  descriptor.primitive.frontFace = wgpu::FrontFace::CCW;
  descriptor.primitive.cullMode = wgpu::CullMode::None;
  descriptor.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
  descriptor.primitive.stripIndexFormat = wgpu::IndexFormat::Undefined;

  pipeline = device.CreateRenderPipeline2(&descriptor);
}

// render_frame is for rendering locally, for debugging
void render_frame() {
  static uint16_t fc = 0; // frame counter
  fc++;
  float RED   = 0.4;
  float GREEN = 0.4;
  float BLUE  = 0.4;
  if (animate) {
    RED   = abs(sinf(float(fc) / 100));
    GREEN = abs(sinf(float(fc) / 90));
    BLUE  = abs(cosf(float(fc) / 80));
  }

  wgpu::TextureView backbufferView = swapchain.GetCurrentTextureView();
  wgpu::RenderPassDescriptor renderpassInfo;
  wgpu::RenderPassColorAttachmentDescriptor colorAttachment;
  colorAttachment.attachment = backbufferView;
  colorAttachment.resolveTarget = nullptr;
  colorAttachment.clearColor = {RED, GREEN, BLUE, 0.0f};
  colorAttachment.loadOp = wgpu::LoadOp::Clear;
  colorAttachment.storeOp = wgpu::StoreOp::Store;
  renderpassInfo.colorAttachmentCount = 1;
  renderpassInfo.colorAttachments = &colorAttachment;
  renderpassInfo.depthStencilAttachment = nullptr;

  wgpu::CommandBuffer commands;
  wgpu::CommandEncoder encoder = device.CreateCommandEncoder(nullptr);
  wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&renderpassInfo);
  pass.SetPipeline(pipeline);
  pass.Draw(3, 1, 0, 0);
  pass.EndPass();
  pass.Release();
  commands = encoder.Finish(nullptr);

  queue.Submit(1, &commands);
  swapchain.Present();

  // if (!c2sBuf->Flush())
  //   dlog("c2sBuf->Flush() failed");
}

// onServerIO is called when a new connection is awaiting accept
static void onServerIO(RunLoop* rl, ev_io* w, int revents) {
  dlog("onServerIO called");
  int fd = accept(w->fd, NULL, NULL);
  if (fd < 0) {
    if (errno != EAGAIN)
      perror("accept");
    return;
  }
  FDSetNonBlock(fd);

  if (conn0 != nullptr) {
    dlog("second client connected; closing older client (last in wins)");
    conn0->close();
  }

  static uint32_t connIdGen = 0;
  conn0 = new Conn(connIdGen++);
  dlog("accepted new connection #%u [fd %d]", conn0->id, fd);
  conn0->proto.start(rl, fd);
}

void onPollTimeout(RunLoop* rl, ev_timer* w, int revents) {
  // dlog("poll timeout");
  ev_timer_again(rl, w);
  // render_frame(); // render locally
  // swapchain.Present();
}

void onFrameTimer(RunLoop* rl, ev_timer* w, int revents) {
  if (conn0) {
    swapchain.Present();
    wgpu::TextureView backbufferView = swapchain.GetCurrentTextureView();
    conn0->proto.writeFrame();
  }
  ev_timer_again(rl, w);
}

int main(int argc, const char* argv[]) {
  dlog("starting UNIX socket server \"%s\"", sockfile);
  int fd = createUNIXSocketServer(sockfile);
  if (fd < 0) {
    perror("createUNIXSocketServer");
    return 1;
  }

  createOSWindow();

  init_dawn();

  RunLoop* rl = EV_DEFAULT;

  // register I/O callback for the socket file descriptor
  FDSetNonBlock(fd);
  ev_io server_fd_watcher;
  ev_io_init(&server_fd_watcher, onServerIO, fd, EV_READ);
  ev_io_start(rl, &server_fd_watcher);

  // use a timer to drive client rendering
  ev_timer frame_timer;
  ev_init(&frame_timer, onFrameTimer);
  frame_timer.repeat = 1.0;
  ev_timer_again(rl, &frame_timer);
  ev_unref(rl); // don't allow timer to keep runloop alive alone

  // use a timer to drive the runloop so we can call glfwPollEvents often enough
  const uint32_t FPS = 60;
  ev_timer timer;
  ev_init(&timer, onPollTimeout);
  timer.repeat = 1.0 / (double)FPS;
  ev_timer_again(rl, &timer);
  ev_unref(rl); // don't allow timer to keep runloop alive alone

  //// for some reason we need to do this once for things to work... why?
  //if (!c2sBuf->Flush())
  //  dlog("c2sBuf->Flush failed");

  // // stats
  // double framestats[FPS] = {0};
  // uint32_t frameCounter = 0;

  while (!glfwWindowShouldClose(window)) {
    //double t1 = glfwGetTime(); // measure time for stats
    glfwPollEvents(); // check for OS events
    ev_run(rl, EVRUN_ONCE); // poll for I/O events

    // // update stats
    // framestats[frameCounter % FPS] = glfwGetTime() - t1;
    // frameCounter++;
    // if ((frameCounter % FPS) == 0) {
    //   double avgtime = 0.0;
    //   for (uint32_t i = 0; i < FPS; i++) {
    //     avgtime += framestats[i];
    //   }
    //   avgtime = avgtime / (double)FPS;
    //   dlog("%.1f ms/frame  (%.0f FPS)", avgtime * 1000.0, 1.0 / avgtime);
    // }
  }

  dlog("exit");
  if (conn0)
    conn0->close();
  ev_io_stop(rl, &server_fd_watcher);
  ev_timer_stop(rl, &timer);
  close(fd);
  unlink(sockfile);
  return 0;
}
