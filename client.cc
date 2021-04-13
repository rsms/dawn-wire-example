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

#include <dawn/webgpu.h>

#include <dawn/dawn_proc.h>
#include <dawn/dawn_wsi.h>
#include <dawn_wire/WireClient.h>
#include <dawn_wire/WireServer.h>

#include <cassert>
#include <cmath>
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

#define DLOG_PREFIX "\e[1;36m[client]\e[0m "

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



#define COMMAND_BUFFER_SIZE 4096*4

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
    // success = mHandler->HandleCommands(mBuffer, mOffset) != nullptr;
    if (w != -1) {
      printf("cmdbuf %s Flush write %zu bytes\n", mName, mOffset);
      ssize_t z = ::write(w, mBuffer, mOffset);
      if (size_t(z) != mOffset) {
        perror("cmdbuf Flush write");
        success = false;
      }
    }
    mOffset = 0;
    return success;
  }
};

static dawn_wire::WireClient* wireClient = nullptr;
static LolCommandBuffer* c2sBuf = nullptr;

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


wgpu::Device createWebGPUDevice() {
  DawnProcTable procs;

  c2sBuf = new LolCommandBuffer("c2s");
  dawn_wire::WireClientDescriptor clientDesc = {};
  clientDesc.serializer = c2sBuf;
  wireClient = new dawn_wire::WireClient(clientDesc);
  procs = dawn_wire::client::GetProcs();

  auto deviceReservation = wireClient->ReserveDevice();

  WGPUDevice cDevice = deviceReservation.device;

  dawnProcSetProcs(&procs);
  procs.deviceSetUncapturedErrorCallback(cDevice, PrintDeviceError, nullptr);
  return wgpu::Device::Acquire(cDevice);
}


void flushWireBuffers() {
  bool c2sSuccess = c2sBuf->Flush();
  ASSERT(c2sSuccess);
}


void configureSwapchain(int width, int height) {
  WGPUSwapChainDescriptor descriptor = {};
  // descriptor.implementation = binding->GetSwapChainImplementation();
  //descriptor.implementation = dawn_native::null::CreateNativeSwapChainImpl();
  descriptor.format = WGPUTextureFormat_RGBA8Unorm;
  descriptor.presentMode = WGPUPresentMode_Immediate;
  swapchain = wgpuDeviceCreateSwapChain(device, nullptr, &descriptor);

  // wgpu::TextureFormat textureFormat = static_cast<wgpu::TextureFormat>(
  //   binding->GetPreferredSwapChainTextureFormat())
  // swapChainFormat = static_cast<WGPUTextureFormat>(textureFormat);

  // swapChainFormat = WGPUTextureFormat_RGBA8Unorm;
  wgpuSwapChainConfigure(swapchain, descriptor.format, WGPUTextureUsage_RenderAttachment,
    width, height);
}


void init_dawn() {
  // device = CreateCppDawnDevice().Release();
  // device = createWebGPUDevice().Release();
  queue = wgpuDeviceGetQueue(device);

  configureSwapchain(640, 480);

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

uint32_t fc = 0;
bool animate = true;


void render_frame() {
  fc++;
  float RED   = 0.4;
  float GREEN = 0.4;
  float BLUE  = 0.4;
  if (animate) {
    RED   = std::abs(sinf(float(fc) / 100));
    GREEN = std::abs(sinf(float(fc) / 90));
    BLUE  = std::abs(cosf(float(fc) / 80));
  }

  WGPUTextureView backbufferView = wgpuSwapChainGetCurrentTextureView(swapchain);
  WGPURenderPassDescriptor renderpassInfo = {};
  WGPURenderPassColorAttachmentDescriptor colorAttachment = {};
  {
    colorAttachment.attachment = backbufferView;
    colorAttachment.resolveTarget = nullptr;
    colorAttachment.clearColor = {RED, GREEN, BLUE, 0.0f};
    colorAttachment.loadOp = WGPULoadOp_Clear;
    colorAttachment.storeOp = WGPUStoreOp_Store;
    renderpassInfo.colorAttachmentCount = 1;
    renderpassInfo.colorAttachments = &colorAttachment;
    renderpassInfo.depthStencilAttachment = nullptr;
  }
  WGPUCommandBuffer commands;
  {
    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device, nullptr);

    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &renderpassInfo);
    wgpuRenderPassEncoderSetPipeline(pass, pipeline);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
    wgpuRenderPassEncoderEndPass(pass);
    wgpuRenderPassEncoderRelease(pass);

    commands = wgpuCommandEncoderFinish(encoder, nullptr);
    wgpuCommandEncoderRelease(encoder);
  }

  wgpuQueueSubmit(queue, 1, &commands);
  wgpuCommandBufferRelease(commands);
  wgpuSwapChainPresent(swapchain);
  wgpuTextureViewRelease(backbufferView);

  if (!c2sBuf->Flush()) // blocks on write I/O
    dlog("c2sBuf->Flush() failed");

  //flushWireBuffers();
}


const char* sockfile = "server.sock";
char rbuf[COMMAND_BUFFER_SIZE];

struct {
  RunLoop* rl;
  ev_io    w;
  bool     gotWelcomeMessage;
} conn;

static void close_connection() {
  assert(conn.rl != nullptr);
  ev_io_stop(conn.rl, &conn.w);
  ::close(conn.w.fd);
}


// client_fd_cb is called when a client's connection has available I/O
static void client_fd_cb(RunLoop* rl, ev_io* w, int revents) {
  dlog("I/O %s %s",
    revents & EV_READ ? "EV_READ" : "",
    revents & EV_WRITE ? "EV_WRITE" : "");

  int fd = w->fd;

  const char* welcomeMessage = "OHAI\n";

  if (revents & EV_READ) {
    ssize_t n = read(fd, rbuf, sizeof(rbuf));
    dlog("read %zd bytes", n);
    if (n == 0) {
      close_connection();
      return;
    }

    if (!conn.gotWelcomeMessage) {
      if ((size_t)n >= strlen(welcomeMessage) ||
          memcmp(rbuf, welcomeMessage, strlen(welcomeMessage)) == 0)
      {
        dlog("received welcome message from server");
        conn.gotWelcomeMessage = true;
        n -= strlen(welcomeMessage);
      } else {
        dlog("expected welcome message but got something else; closing connection");
        close_connection();
        return;
      }
    }

    if (n > 0) {
      // feed data to wire client
      bool success = wireClient->HandleCommands(rbuf, (size_t)n) != nullptr;
      dlog("wireClient->HandleCommands => %s", success ? "ok" : "fail");
    }
  }
}

static void client_poll_timeout_cb(RunLoop* rl, ev_timer* w, int revents) {
  dlog("render");
  double t = ev_time();
  render_frame();
  t = ev_time() - t;
  dlog("frame time: %.2f ms", t * 1000.0);

  ev_timer_again(rl, w);
}

void runloop_main(int fd) {
  RunLoop* rl = EV_DEFAULT;

  device = createWebGPUDevice().Release();
  c2sBuf->w = fd;

  init_dawn();

  ::memset(&conn, 0, sizeof(conn));
  conn.rl = rl;

  FDSetNonBlock(fd);
  ev_io_init(&conn.w, client_fd_cb, fd, EV_READ);
  ev_io_start(rl, &conn.w);

  ev_timer timeout_w;
  ev_init(&timeout_w, client_poll_timeout_cb);
  timeout_w.repeat = 1.0; //1.0 / 60.0;
  ev_timer_again(rl, &timeout_w);
  ev_unref(rl); // don't allow timer to keep runloop alive alone

  // returns when the connection closes (when there are no more watchers)
  while (1) {
    if (ev_run(rl, EVRUN_ONCE) == 0) {
      // no more active watchers
      dlog("io: no active watchers -- exit runloop");
      break;
    }
  }
}

int main(int argc, const char* argv[]) {
  while (1) {
    dlog("connecting to UNIX socket \"%s\"", sockfile);
    int fd = connectUNIXSocket(sockfile);
    if (fd < 0) {
      perror("connectUNIXSocket");
      sleep(1);
      continue;
    }
    dlog("connected to socket");
    runloop_main(fd);
    close(fd);
  }
  dlog("exit");
  return 0;
}
