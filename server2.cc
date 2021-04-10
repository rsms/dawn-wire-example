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
#include "utils/TerribleCommandBuffer.h"

#include "utils/SystemUtils.h"
#include "utils/WGPUHelpers.h"
#include "GLFW/glfw3.h"

#include <dawn/webgpu.h>

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

#define DLOG_PREFIX "\e[1;34m[server]\e[0m "

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


enum class CmdBufType {
  None,
  Terrible,
};

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

static CmdBufType                             cmdBufType = CmdBufType::Terrible;
static std::unique_ptr<dawn_native::Instance> instance;
static utils::BackendBinding*                 binding = nullptr;
static GLFWwindow*                            window = nullptr;

static dawn_wire::WireServer* wireServer = nullptr;
static dawn_wire::WireClient* wireClient = nullptr;
static LolCommandBuffer* c2sBuf = nullptr;
static LolCommandBuffer* s2cBuf = nullptr;

float uiScale = 1.0;


static WGPUDevice         device;
static WGPUQueue          queue;
static WGPUSwapChain      swapchain;
static WGPURenderPipeline pipeline;
static WGPUTextureFormat  swapChainFormat;


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


wgpu::Device CreateCppDawnDevice2() {
  //if (GetEnvironmentVar("ANGLE_DEFAULT_PLATFORM").empty()) {
  //  SetEnvironmentVar("ANGLE_DEFAULT_PLATFORM", "swiftshader");
  //}

  glfwSetErrorCallback(PrintGLFWError);
  if (!glfwInit()) {
    return wgpu::Device();
  }

  // Create the test window and discover adapters using it (esp. for OpenGL)
  utils::SetupGLFWWindowHintsForBackend(backendType);
  glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_FALSE);
  GLFWmonitor* monitor = nullptr;
  window = glfwCreateWindow(640, 480, "hello-wire", monitor, nullptr);
  if (!window)
    return wgpu::Device();

  // read window UI scale from OS
  float yscale = 0.0; // ignored
  glfwGetWindowContentScale(window, &uiScale, &yscale);

  // [rsms] move window to bottom right corner of screen
  // glfwSetWindowPos(window, 1920, 960);
  glfwSetWindowPos(window, 2560, 960); // 2nd screen, bottom left corner

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
  if (binding == nullptr) {
    return wgpu::Device();
  }

  // Choose whether to use the backend procs and devices directly, or set up the wire.
  WGPUDevice cDevice = nullptr;
  DawnProcTable procs;

  switch (cmdBufType) {
    case CmdBufType::None:
      procs = backendProcs;
      cDevice = backendDevice;
      break;

    case CmdBufType::Terrible: {
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

      procs = dawn_wire::client::GetProcs();

      auto devres = wireClient->ReserveDevice();
      wireServer->InjectDevice(backendDevice, devres.id, devres.generation);
      cDevice = devres.device;
    } break;
  }

  dawnProcSetProcs(&procs);
  procs.deviceSetUncapturedErrorCallback(cDevice, PrintDeviceError, nullptr);
  return wgpu::Device::Acquire(cDevice);
}


void flushWireBuffers() {
  if (cmdBufType == CmdBufType::Terrible) {
    bool s2cSuccess = s2cBuf->Flush();
    ASSERT(s2cSuccess);
  }
}


wgpu::TextureFormat GetPreferredSwapChainTextureFormat2() {
  flushWireBuffers();
  return static_cast<wgpu::TextureFormat>(binding->GetPreferredSwapChainTextureFormat());
}


void configureSwapchain(int width, int height) {
  WGPUSwapChainDescriptor descriptor = {};
  descriptor.implementation = binding->GetSwapChainImplementation();
  swapchain = wgpuDeviceCreateSwapChain(device, nullptr, &descriptor);
  swapChainFormat = static_cast<WGPUTextureFormat>(GetPreferredSwapChainTextureFormat2());
  wgpuSwapChainConfigure(swapchain, swapChainFormat, WGPUTextureUsage_RenderAttachment,
    width, height);
}


void init_dawn() {
  // device = CreateCppDawnDevice().Release();
  device = CreateCppDawnDevice2().Release();
  queue = wgpuDeviceGetQueue(device);

  int width_px = 100;
  int height_px = 100;
  glfwGetFramebufferSize(window, &width_px, &height_px);
  configureSwapchain(width_px, height_px);

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
  WGPUShaderModule vsModule = utils::CreateShaderModule(device, vs).Release();

  const char* fs =
    "[[location(0)]] var<out> fragColor : vec4<f32>;\n"
    "[[stage(fragment)]] fn main() -> void {\n"
    "    fragColor = vec4<f32>(1.0, 0.0, 0.7, 1.0);\n"
    "    return;\n"
    "}\n";
  WGPUShaderModule fsModule = utils::CreateShaderModule(device, fs).Release();

  {
    WGPURenderPipelineDescriptor2 descriptor = {};

    // Fragment state
    WGPUBlendState blend = {};
    blend.color.operation = WGPUBlendOperation_Add;
    blend.color.srcFactor = WGPUBlendFactor_One;
    blend.color.dstFactor = WGPUBlendFactor_One;
    blend.alpha.operation = WGPUBlendOperation_Add;
    blend.alpha.srcFactor = WGPUBlendFactor_One;
    blend.alpha.dstFactor = WGPUBlendFactor_One;

    WGPUColorTargetState colorTarget = {};
    colorTarget.format = swapChainFormat;
    colorTarget.blend = &blend;
    colorTarget.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState fragment = {};
    fragment.module = fsModule;
    fragment.entryPoint = "main";
    fragment.targetCount = 1;
    fragment.targets = &colorTarget;
    descriptor.fragment = &fragment;

    // Other state
    descriptor.layout = nullptr;
    descriptor.depthStencil = nullptr;

    descriptor.vertex.module = vsModule;
    descriptor.vertex.entryPoint = "main";
    descriptor.vertex.bufferCount = 0;
    descriptor.vertex.buffers = nullptr;

    descriptor.multisample.count = 1;
    descriptor.multisample.mask = 0xFFFFFFFF;
    descriptor.multisample.alphaToCoverageEnabled = false;

    descriptor.primitive.frontFace = WGPUFrontFace_CCW;
    descriptor.primitive.cullMode = WGPUCullMode_None;
    descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    descriptor.primitive.stripIndexFormat = WGPUIndexFormat_Undefined;

    pipeline = wgpuDeviceCreateRenderPipeline2(device, &descriptor);
  }

  wgpuShaderModuleRelease(vsModule);
  wgpuShaderModuleRelease(fsModule);
}

bool animate = false;

// windowOnKeyPress is called when keyboard keys are pressed.
//   window   The window that received the event.
//   key      The keyboard key that was pressed or released. (GLFW_KEY_*)
//   scancode The system-specific scancode of the key.
//   action   One of: GLFW_PRESS, GLFW_RELEASE, GLFW_REPEAT
//   mods     Bit field describing which modifier keys were held down. (GLFW_MOD_*)
//
void windowOnKeyPress(GLFWwindow* window, int key, int scancode, int action, int mods) {
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

void frame();

// onWindowFramebufferResize is called when a window's framebuffer has changed size
//   window The window which framebuffer was resized.
//   width  The new width, in pixels, of the framebuffer.
//   height The new height, in pixels, of the framebuffer.
void onWindowFramebufferResize(GLFWwindow* window, int width, int height) {
  //printf("onWindowFramebufferResize width=%d, height=%d\n", width, height);
  //configureSwapchain(width, height);
}

// onWindowResize is called when a window has been resized
//   window The window that was resized.
//   width  The new width, in screen coordinates, of the window.
//   height The new height, in screen coordinates, of the window.
// Note: onWindowResize is called after any call to onWindowFramebufferResize
void onWindowResize(GLFWwindow* window, int width, int height) {
  //printf("onWindowResize width=%d, height=%d\n", width, height);
  // redraw as onWindowFramebufferResize might have replaced the swapchain
  // frame();
}



// Client is a connection to a remote client (from the server's perspective)
struct Client {
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
Client* server_client0 = nullptr;

// server_fd_cb is called when a server's connection to a client has available I/O
static void server_client_fd_cb(RunLoop* rl, ev_io* w, int revents) {
  Client* client = (Client*)w->data;
  // dlog("server_client_fd_cb %s %s",
  //   revents & EV_READ ? "EV_READ" : "",
  //   revents & EV_WRITE ? "EV_WRITE" : "");

  int fd = client->fd();

  if (revents & EV_READ) {
    char rbuf[COMMAND_BUFFER_SIZE];
    ssize_t n = ::read(fd, rbuf, sizeof(rbuf));
    //dlog("read %zd bytes", n);
    if (n == 0) {
      dlog("client#%u gone", client->id);
      client->close();
      // delete client;
      // if (client == server_client0)
      //   server_client0 = nullptr;
      return;
    }
    // handle incoming data from client
    if (wireServer->HandleCommands(rbuf, (size_t)n) == nullptr)
      dlog("wireServer->HandleCommands FAILED");
  }

  if (revents & EV_WRITE) {
    auto& b = client->wbuf;
    if (b.len != 0) {
      ssize_t z = ::write(fd, &b.p[b.len], b.len);
      dlog("server_client_fd_cb write(%zu) => %zd", b.len, z);
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

// server_fd_cb is called when data is readable, i.e. when a connection is awaiting accept
static void server_fd_cb(RunLoop* rl, ev_io* w, int revents) {
  dlog("server_fd_cb called");
  int fd = accept(w->fd, NULL, NULL);
  if (fd < 0) {
    if (errno != EAGAIN)
      perror("accept");
    return;
  }
  FDSetNonBlock(fd);

  Client* client = new Client();
  server_client0 = client;
  static uint32_t clientIdGen = 0;
  client->id = clientIdGen++;
  client->rl = rl;
  client->io.data = (void*)client;
  dlog("accepted new connection client#%u [fd %d]", client->id, fd);
  //s2cBuf->w = fd;
  ev_io_init(&client->io, server_client_fd_cb, fd, EV_READ);
  ev_io_start(rl, &client->io);

  // send welcome message
  client->write("OHAI\n", 5);

  // close(fd);
}

// another callback, this time for a time-out
static void server_poll_timeout_cb(RunLoop* rl, ev_timer* w, int revents) {
  // dlog("poll timeout");
  // w->repeat = 2.0;
  // ev_timer_again(rl, w);
  // ev_timer_stop(rl, w);
}

void server_runloop(int fd) {
  dlog("main start");
  RunLoop* rl = EV_DEFAULT;

  FDSetNonBlock(fd);
  ev_io server_fd_watcher;
  ev_io_init(&server_fd_watcher, server_fd_cb, fd, EV_READ);
  ev_io_start(rl, &server_fd_watcher);

  ev_timer timeout_w;
  ev_init(&timeout_w, server_poll_timeout_cb);

  const uint32_t FPS = 5;
  // double frameTimeGoal = 1.0 / 60.0;
  double frameTimeGoal = 1.0 / (double)FPS;
  timeout_w.repeat = frameTimeGoal;
  ev_timer_again(rl, &timeout_w);
  ev_unref(rl); // don't allow timer to keep runloop alive alone

  const uint32_t frameTimingsSize = FPS;
  double frameTimings[2][frameTimingsSize] = {{0}};
  uint32_t frameCounter = 0;

  // for some reason we need to do this once for things to work... why?
  if (!c2sBuf->Flush())
    dlog("c2sBuf->Flush failed");

  // forever
  while (!glfwWindowShouldClose(window) /*&& frameCounter < 10*/) {
    // dlog("frame %u", frameCounter);
    double t1 = glfwGetTime();
    // if (server_client0)
    //   server_client0->write("SYNC\n", 5);

    // frame();

    //if (!c2sBuf->Flush())
    //  dlog("c2sBuf->Flush failed");

    // dlog("frame %u", frameCounter);
    // bool s2cSuccess = s2cBuf->Flush();
    // assert(s2cSuccess);

    double frameTime0 = glfwGetTime() - t1;

    glfwPollEvents();

    timeout_w.repeat = frameTimeGoal - (glfwGetTime() - t1);
    if (timeout_w.repeat > 0.0) {
      ev_timer_again(rl, &timeout_w);
      ev_run(rl, EVRUN_ONCE);
    } else {
      ev_timer_stop(rl, &timeout_w);
      ev_run(rl, EVRUN_NOWAIT);
    }

    double frameTime1 = glfwGetTime() - t1;

    // update stats
    frameTimings[0][frameCounter % frameTimingsSize] = frameTime0;
    frameTimings[1][frameCounter % frameTimingsSize] = frameTime1;
    frameCounter++;
    if ((frameCounter % frameTimingsSize) == 0) {
      double frameTimingsAvg[2] = {0.0, 0.0};
      for (uint32_t i = 0; i < frameTimingsSize; i++) {
        frameTimingsAvg[0] += frameTimings[0][i];
        frameTimingsAvg[1] += frameTimings[1][i];
      }
      frameTimingsAvg[0] = frameTimingsAvg[0] / (double)frameTimingsSize;
      frameTimingsAvg[1] = frameTimingsAvg[1] / (double)frameTimingsSize;
      dlog("render %.2f ms   %.0f FPS (%.2f ms/frame)",
        frameTimingsAvg[0] * 1000.0,
        1 / frameTimingsAvg[1],
        frameTimingsAvg[1] * 1000.0);
    }
  }

  dlog("exit");
  if (server_client0)
    server_client0->close();
  close(fd);
}

int main(int argc, const char* argv[]) {
  // create socket server
  dlog("starting UNIX socket server \"%s\"", sockfile);
  int server_fd = createUNIXSocketServer(sockfile);
  if (server_fd < 0) {
    perror("createUNIXSocketServer");
    return 1;
  }

  // init dawn
  init_dawn();
  glfwSetKeyCallback(window, windowOnKeyPress);
  glfwSetFramebufferSizeCallback(window, onWindowFramebufferResize);
  glfwSetWindowSizeCallback(window, onWindowResize);

  // run server on the this current thread
  server_runloop(server_fd);

  unlink(sockfile);
}

  // // server read loop
  // char buf[256];
  // while (1) {
  //   printf("server calling read(server.r) ...\n");
  //   int r = ::read(server.r, buf, 256);
  //   printf("server read() => %d\n", r);
  //   if (r < 1) {
  //     if (r == -1)
  //       perror("server read");
  //     // I/O closed; exit client
  //     break;
  //   }
  // }

// Note: To update render size when window changes, poll window with glfwGetFramebufferSize
// and retrieve an updated swapchain via wgpuDeviceCreateSwapChain.
// See SyncFromWindow() in dawn/examples/ManualSwapChainTest.cpp for an implementation.
