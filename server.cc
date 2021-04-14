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

#include "utils/GLFWUtils.h"
#include "GLFW/glfw3.h"

#include <dawn/webgpu_cpp.h>
#include <dawn/dawn_proc.h>
#include <dawn_wire/WireServer.h>
#include <dawn_native/DawnNative.h>

#include <algorithm>
#include <cmath>
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


const char* sockfile = "server.sock";
static GLFWwindow* window = nullptr;
static std::unique_ptr<dawn_native::Instance> instance;

DawnProcTable        nativeProcs;
dawn_native::Adapter backendAdapter;
wgpu::Device         device;
wgpu::Surface        surface;
wgpu::SwapChain      swapchain;

DawnRemoteProtocol::FramebufferInfo framebufferInfo = {
  .dpscale = 1000, // 1000=100%
  .width = 640,
  .height = 480,
  .textureFormat = wgpu::TextureFormat::BGRA8Unorm,
  .textureUsage = wgpu::TextureUsage::RenderAttachment,
};


// Conn is a connection to a client
struct Conn {
  uint32_t              id;
  DawnRemoteProtocol    _proto;
  dawn_wire::WireServer _wireServer;

  Conn(uint32_t id_) :
    id(id_),
    _wireServer({ .procs = &nativeProcs, .serializer = &_proto })
  {
    _proto.onDawnBuffer = [this](const char* data, size_t len) {
      dlog("onDawnBuffer len=%zu", len);
      assert(data != nullptr);
      if (_wireServer.HandleCommands(data, len) == nullptr)
        dlog("_wireServer.HandleCommands FAILED");
      if (!_proto.Flush())
        dlog("_proto.Flush() FAILED");
    };

    _proto.onSwapchainReservation = [this](const dawn_wire::ReservedSwapChain& scr) {
      dlog("onSwapchainReservation");
      // if (!_wireServer.InjectDevice(device.Get(), scr.deviceId, scr.deviceGeneration)) {
      //   dlog("onDeviceReservation _wireServer.InjectDevice FAILED");
      //   return;
      // }
      if (!_wireServer.InjectSwapChain(
        swapchain.Get(), scr.id, scr.generation, scr.deviceId, scr.deviceGeneration))
      {
        dlog("onSwapchainReservation _wireServer.InjectSwapChain FAILED");
      }
    };

    // Hardcoded generation and IDs need to match what's produced by the client
    // or be sent over through the wire.
    _wireServer.InjectDevice(device.Get(), 1, 0);
    _wireServer.InjectSwapChain(swapchain.Get(), 1, 0, 1, 0);
    // kangz: Maybe we should inject the surface instead and just let the client
    // create its swapchain??
  }

  void start(RunLoop* rl, int fd) {
    _proto.start(rl, fd);
  }

  bool sendFramebufferInfo() {
    if (_proto.stopped())
      return false;
    return _proto.sendFramebufferInfo(framebufferInfo);
  }

  bool sendFrameSignal() {
    if (_proto.stopped()) {
      close();
      return false;
    }
    // send FRAME message to client
    if (!_proto.sendFrameSignal()) {
      dlog("_proto.sendFrameSignal FAILED");
      close();
      return false;
    }
    return true;
  }

  void close() {
    _proto.stop();
    if (_proto.fd() != -1)
      ::close(_proto.fd());
  }
};

Conn* conn0 = nullptr;

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
      assert(false);
      return;
  }
  std::cerr << "device error: " << errorTypeName << " error: " << message << std::endl;
}

static void PrintGLFWError(int code, const char* message) {
  std::cerr << "GLFW error: " << code << " - " << message << std::endl;
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

// logAvailableAdapters prints a list of all adapters and their properties
void logAvailableAdapters(dawn_native::Instance* instance) {
  fprintf(stderr, "Available adapters:\n");
  for (auto&& a : instance->GetAdapters()) {
    wgpu::AdapterProperties p;
    a.GetProperties(&p);
    fprintf(stderr, "  %s (%s)\n"
      "    deviceID=%u, vendorID=0x%x, BackendType::%s, AdapterType::%s\n",
      p.name, p.driverDescription,
      p.deviceID, p.vendorID, backendTypeName(p.backendType), adapterTypeName(p.adapterType));
  }
}

// updates values of the global variable framebufferInfo
void updateFramebufferInfo(uint32_t width, uint32_t height) {
  framebufferInfo.width = width;
  framebufferInfo.height = height;
  float xscale, yscale;
  glfwGetWindowContentScale(window, &xscale, &yscale);
  framebufferInfo.dpscale = (uint16_t)std::min((double)0xFFFF, (double)xscale * 1000.0);
}

void createDawnSwapChain();

// onWindowFramebufferResize is called when a window's framebuffer has changed size
// width & height are in pixels (the framebuffer size)
void onWindowFramebufferResize(GLFWwindow* window, int width, int height) {
  dlog("onWindowFramebufferResize width=%d, height=%d", width, height);
  // [WORK IN PROGRESS]
  // // update framebuffer info and swapchain
  // updateFramebufferInfo((uint32_t)width, (uint32_t)height);
  // createDawnSwapChain(); // note: can't use swapchain.Configure when we use a surface
  // if (conn0)
  //   conn0->sendFramebufferInfo();
}

// onWindowResize is called when a window has been resized
// Note: onWindowResize is called _after_ onWindowFramebufferResize has been called.
// width & height are in screen coordinates (i.e. dp)
void onWindowResize(GLFWwindow* window, int width, int height) {
  // dlog("onWindowResize width=%d, height=%d", width, height);
}

void createOSWindow() {
  assert(window == nullptr);

  glfwSetErrorCallback(PrintGLFWError);
  if (!glfwInit())
    return;

  // Setup the correct hints for GLFW for backends.
  utils::SetupGLFWWindowHintsForBackend(backendType);
  glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_FALSE);
  window = glfwCreateWindow(
    framebufferInfo.width,
    framebufferInfo.height,
    "hello-wire",
    /*monitor*/nullptr,
    nullptr);

  if (!window)
    return;

  // get actual framebuffer size
  int width, height;
  glfwGetFramebufferSize(window, &width, &height);
  updateFramebufferInfo((uint32_t)width, (uint32_t)height);

  glfwSetFramebufferSizeCallback(window, onWindowFramebufferResize);
  glfwSetWindowSizeCallback(window, onWindowResize);
}

void createDawnDevice() {
  instance = std::make_unique<dawn_native::Instance>();
  instance->DiscoverDefaultAdapters();

  logAvailableAdapters(instance.get());

  // Get an adapter for the backend to use, and create the device.
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
    assert(adapterIt != adapters.end());
    backendAdapter = *adapterIt; // global var
  }

  // Set up the native procs for the global proctable (so calling
  // wgpu::Object::Foo calls into that proc) but also keep it around
  // so we can give it to the wire server.
  nativeProcs = dawn_native::GetProcs(); // global var
  dawnProcSetProcs(&nativeProcs);

  device = wgpu::Device::Acquire(backendAdapter.CreateDevice());// global var

  // hook up error reporting
  device.SetUncapturedErrorCallback(PrintDeviceError, nullptr);
}

void createDawnSwapChain() {
  surface = utils::CreateSurfaceForWindow(instance->Get(), window); // global var
  wgpu::SwapChainDescriptor desc = {
    .format = framebufferInfo.textureFormat,
    .usage  = framebufferInfo.textureUsage,
    .width  = framebufferInfo.width,
    .height = framebufferInfo.height,
    .presentMode = wgpu::PresentMode::Mailbox,
  };
  swapchain = device.CreateSwapChain(surface, &desc); // global var
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
  conn0->start(rl, fd);
  conn0->sendFramebufferInfo();
}

void onPollTimeout(RunLoop* rl, ev_timer* w, int revents) {
  // dlog("poll timeout");
  ev_timer_again(rl, w);
}

void onFrameTimer(RunLoop* rl, ev_timer* w, int revents) {
  if (conn0) {
    if (!conn0->sendFrameSignal()) {
      // connection closed
      delete conn0;
      conn0 = nullptr;
    }
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
  createDawnDevice();
  createDawnSwapChain();

  RunLoop* rl = EV_DEFAULT;

  // register I/O callback for the socket file descriptor
  FDSetNonBlock(fd);
  ev_io server_fd_watcher;
  ev_io_init(&server_fd_watcher, onServerIO, fd, EV_READ);
  ev_io_start(rl, &server_fd_watcher);

  // use a timer to drive client rendering
  ev_timer frame_timer;
  ev_init(&frame_timer, onFrameTimer);
  frame_timer.repeat = 1.0 / 10.0;
  ev_timer_again(rl, &frame_timer);
  ev_unref(rl); // don't allow timer to keep runloop alive alone

  // use a timer to drive the runloop so we can call glfwPollEvents often enough
  ev_timer timer;
  ev_init(&timer, onPollTimeout);
  timer.repeat = 1.0 / 60.0;
  ev_timer_again(rl, &timer);
  ev_unref(rl); // don't allow timer to keep runloop alive alone

  while (!glfwWindowShouldClose(window)) {
    //double t1 = glfwGetTime(); // measure time for stats
    glfwPollEvents(); // check for OS events
    ev_run(rl, EVRUN_ONCE); // poll for I/O events
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
