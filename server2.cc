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

// silence "mangled name of 'ev_set_allocator' will change in C++17"
_Pragma("GCC diagnostic push")
_Pragma("GCC diagnostic ignored \"-Wc++17-compat-mangling\"")
#include <ev.h>
_Pragma("GCC diagnostic pop")

typedef struct ev_loop RunLoop;

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


#define COMMAND_BUFFER_SIZE 4096*32

class LolCommandBuffer : public dawn_wire::CommandSerializer {
  dawn_wire::CommandHandler* mHandler = nullptr;
  size_t                     mOffset = 0;
  char                       mBuffer[COMMAND_BUFFER_SIZE];
  const char*                mName = "";
public:
  int w = -1; // file descriptor to write to

  LolCommandBuffer(const char* name) : mName(name) {}
  LolCommandBuffer(dawn_wire::CommandHandler* handler) : mHandler(handler) {}

  void SetHandler(dawn_wire::CommandHandler* handler) { mHandler = handler; }

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
    if (mOffset == 0)
      return true;
    bool success = true;
    success = mHandler->HandleCommands(mBuffer, mOffset) != nullptr;
    if (!success) {
      dlog("cmd buffer %s HandleCommands (%zu) FAILED", mName, mOffset);
    } else {
      dlog("cmd buffer %s HandleCommands (%zu) OK", mName, mOffset);
    }
    if (w != -1) {
      dlog("cmd buffer %s Flush write %zu bytes", mName, mOffset);
      ssize_t z = ::write(w, mBuffer, mOffset);
      if (size_t(z) != mOffset) {
        perror("cmd buffer Flush write");
        success = false;
      }
    }
    //else {
    //  dlog("cmd buffer %s Flush skipping write since w=-1", mName);
    //}
    mOffset = 0;
    return success;
  }
};

static std::unique_ptr<dawn_native::Instance> instance;
static utils::BackendBinding*                 binding = nullptr;
static GLFWwindow*                            window = nullptr;

static dawn_wire::WireServer* wireServer = nullptr;
static dawn_wire::WireClient* wireClient = nullptr;
static LolCommandBuffer* c2sBuf = nullptr;
static LolCommandBuffer* s2cBuf = nullptr;

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


void flushWireBuffers() {
  bool s2cSuccess = s2cBuf->Flush();
  ASSERT(s2cSuccess);
}


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
  flushWireBuffers();
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


wgpu::Device createDawnDevice() {
  if (window == nullptr)
    return wgpu::Device();

  instance = std::make_unique<dawn_native::Instance>();
  utils::DiscoverAdapter(instance.get(), window, backendType);

  // Get an adapter for the backend to use, and create the device.
  dawn_native::Adapter backendAdapter;
  {
    std::vector<dawn_native::Adapter> adapters = instance->GetAdapters();
    auto adapterIt = std::find_if(adapters.begin(), adapters.end(),
      [](const dawn_native::Adapter adapter) -> bool {
        wgpu::AdapterProperties properties;
        adapter.GetProperties(&properties);
        return properties.backendType == backendType;
      });
    ASSERT(adapterIt != adapters.end());
    backendAdapter = *adapterIt;
  }

  WGPUDevice backendDevice = backendAdapter.CreateDevice();
  DawnProcTable backendProcs = dawn_native::GetProcs();

  binding = utils::CreateBinding(backendType, window, backendDevice);
  if (binding == nullptr)
    return wgpu::Device();

  c2sBuf = new LolCommandBuffer("c2s");
  s2cBuf = new LolCommandBuffer("s2c");

  dawn_wire::WireServerDescriptor serverDesc = {};
  serverDesc.procs = &backendProcs;
  serverDesc.serializer = s2cBuf;
  wireServer = new dawn_wire::WireServer(serverDesc);
  c2sBuf->SetHandler(wireServer);

  dawn_wire::WireClientDescriptor clientDesc = {};
  clientDesc.serializer = c2sBuf;
  wireClient = new dawn_wire::WireClient(clientDesc);
  s2cBuf->SetHandler(wireClient);

  DawnProcTable procs = dawn_wire::client::GetProcs();

  dawn_wire::ReservedDevice devReservation = wireClient->ReserveDevice();
  wireServer->InjectDevice(backendDevice, devReservation.id, devReservation.generation);
  WGPUDevice cDevice = devReservation.device;

  dawnProcSetProcs(&procs);
  procs.deviceSetUncapturedErrorCallback(cDevice, PrintDeviceError, nullptr);
  return wgpu::Device::Acquire(cDevice);
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

  if (!c2sBuf->Flush())
    dlog("c2sBuf->Flush() failed");
}



// Conn is a connection to a client
struct Conn {
  uint32_t id;
  RunLoop* rl;
  ev_io    io;
  bool     shutdown = false; // true if shutting down

  struct {
    char*  p[COMMAND_BUFFER_SIZE];
    size_t len = 0;
  } wbuf;

  int fd() { return io.fd; }

  bool write(const void* ptr, size_t len) {
    if (len > 0) {
      if (shutdown || len > COMMAND_BUFFER_SIZE - wbuf.len)
        return false;
      memcpy(&wbuf.p[wbuf.len], ptr, len);
      wbuf.len += len;
      dlog("wrote %zu bytes to client connection", len);
      if ((io.events & EV_WRITE) == 0)
        ev_io_modify(&io, io.events | EV_WRITE);
    }
    return true;
  }

  void close() {
    if (rl != nullptr) {
      ev_io_stop(rl, &io);
      rl = nullptr;
    }
    if (io.fd != -1) {
      ::close(io.fd);
      io.fd = -1;
    }
  }
};

const char* sockfile = "server.sock";
Conn* conn0 = nullptr;

// onConnIO is called when a client connection has available I/O
static void onConnIO(RunLoop* rl, ev_io* w, int revents) {
  Conn* conn = (Conn*)w->data;
  // dlog("onConnIO %s %s",
  //   revents & EV_READ ? "EV_READ" : "",
  //   revents & EV_WRITE ? "EV_WRITE" : "");

  int fd = conn->fd();

  if (revents & EV_READ) {
    char rbuf[COMMAND_BUFFER_SIZE];
    ssize_t n = ::read(fd, rbuf, sizeof(rbuf));
    //dlog("read %zd bytes", n);
    if (n == 0) {
      dlog("connection #%u gone", conn->id);
      conn->close();
      if (conn == conn0)
        conn0 = nullptr;
      delete conn;
      return;
    }
    // handle incoming data from conn
    if (wireServer->HandleCommands(rbuf, (size_t)n) == nullptr)
      dlog("wireServer->HandleCommands FAILED");
  }

  if (revents & EV_WRITE) {
    auto& b = conn->wbuf;
    if (b.len != 0) {
      ssize_t z = ::write(fd, &b.p[b.len], b.len);
      dlog("onConnIO write(%zu) => %zd", b.len, z);
      if (z < b.len) {
        // shift remaining to 0
        size_t len2 = b.len - size_t(z);
        memcpy(b.p, &b.p[b.len], len2);
        b.len = len2;
      } else {
        b.len = 0;
      }
    }
    if (b.len == 0) {
      // nothing to write; stop requesting EV_WRITE
      ev_io_stop(rl, w);
      ev_io_modify(w, w->events & ~EV_WRITE);
      ev_io_start(rl, w);
    }
  }
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
    dlog("ignoring second client");
    close(fd);
    return;
  }

  Conn* conn = new Conn();
  conn0 = conn;
  static uint32_t connIdGen = 0;
  conn->id = connIdGen++;
  conn->rl = rl;
  conn->io.data = (void*)conn;
  dlog("accepted new connection #%u [fd %d]", conn->id, fd);
  //s2cBuf->w = fd;
  ev_io_init(&conn->io, onConnIO, fd, EV_READ);
  ev_io_start(rl, &conn->io);

  // send welcome message
  conn->write("OHAI\n", 5);
}

static void onPollTimeout(RunLoop* rl, ev_timer* w, int revents) {
  // dlog("poll timeout");
  ev_timer_again(rl, w);
  // render_frame(); // render locally
  // swapchain.Present();
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

  // use a timer to drive the runloop so we can call glfwPollEvents often enough
  const uint32_t FPS = 60;
  ev_timer timer;
  ev_init(&timer, onPollTimeout);
  timer.repeat = 1.0 / (double)FPS;
  ev_timer_again(rl, &timer);
  ev_unref(rl); // don't allow timer to keep runloop alive alone

  // for some reason we need to do this once for things to work... why?
  if (!c2sBuf->Flush())
    dlog("c2sBuf->Flush failed");

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
