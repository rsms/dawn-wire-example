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
#include "utils/TerribleCommandBuffer.h"

#include "utils/SystemUtils.h"
#include "utils/WGPUHelpers.h"

#include <dawn/webgpu.h>

#include <dawn/dawn_proc.h>
#include <dawn/dawn_wsi.h>
#include <dawn_wire/WireClient.h>
#include <dawn_wire/WireServer.h>

#include <iostream>

#include <unistd.h> // pipe
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h> // F_GETFL, O_NONBLOCK etc


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


#define MAX(a,b) \
  ({__typeof__ (a) _a = (a); __typeof__ (b) _b = (b); _a > _b ? _a : _b; })

#define MIN(a,b) \
  ({__typeof__ (a) _a = (a); __typeof__ (b) _b = (b); _a < _b ? _a : _b; })


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


DawnClientServerProtocol proto;


static dawn_wire::WireClient* wireClient = nullptr;
// static LolCommandBuffer* c2sBuf = nullptr;

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

  //c2sBuf = new LolCommandBuffer("c2s");
  dawn_wire::WireClientDescriptor clientDesc = {};
  clientDesc.serializer = &proto;
  wireClient = new dawn_wire::WireClient(clientDesc);
  procs = dawn_wire::client::GetProcs();

  auto deviceReservation = wireClient->ReserveDevice();

  WGPUDevice cDevice = deviceReservation.device;

  dawnProcSetProcs(&procs);
  procs.deviceSetUncapturedErrorCallback(cDevice, PrintDeviceError, nullptr);
  return wgpu::Device::Acquire(cDevice);
}


void flushWireBuffers() {
  // bool c2sSuccess = c2sBuf->Flush();
  bool c2sSuccess = proto.Flush();
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


void render_frame(WGPUTexture reservedTexture) {
  fc++;
  float RED   = 0.4;
  float GREEN = 0.4;
  float BLUE  = 0.4;
  if (animate) {
    RED   = abs(sinf(float(fc) / 100));
    GREEN = abs(sinf(float(fc) / 90));
    BLUE  = abs(cosf(float(fc) / 80));
  }

  WGPUTextureView backbufferView = wgpuSwapChainGetCurrentTextureView(swapchain);
  // WGPUTextureViewDescriptor textDescr = {
  //   .label = "a",
  //   .format = WGPUTextureFormat_RGBA8Unorm,
  //   .dimension = WGPUTextureViewDimension_2D,
  //   .baseMipLevel = 1,
  //   .mipLevelCount = 1,
  //   .baseArrayLayer = 0,
  //   .arrayLayerCount = 0,
  //   .aspect = WGPUTextureAspect_All,
  // };
  // WGPUTextureView backbufferView = wgpuTextureCreateView(reservedTexture, &textDescr);

  WGPURenderPassDescriptor renderpassInfo = {};
  WGPURenderPassColorAttachmentDescriptor colorAttachment = {};
  colorAttachment.attachment = backbufferView;
  colorAttachment.resolveTarget = nullptr;
  colorAttachment.clearColor = {RED, GREEN, BLUE, 0.0f};
  colorAttachment.loadOp = WGPULoadOp_Clear;
  colorAttachment.storeOp = WGPUStoreOp_Store;
  renderpassInfo.colorAttachmentCount = 1;
  renderpassInfo.colorAttachments = &colorAttachment;
  renderpassInfo.depthStencilAttachment = nullptr;

  WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device, nullptr);
  WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &renderpassInfo);
  wgpuRenderPassEncoderSetPipeline(pass, pipeline);
  wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
  wgpuRenderPassEncoderEndPass(pass);
  wgpuRenderPassEncoderRelease(pass);
  WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
  wgpuCommandEncoderRelease(encoder);

  wgpuQueueSubmit(queue, 1, &commands);
  wgpuCommandBufferRelease(commands);
  // wgpuSwapChainPresent(swapchain);
  wgpuTextureViewRelease(backbufferView);

  // // tell server we are writing a new frame
  // int fd = c2sBuf->w;
  // ssize_t z = ::write(fd, "FRAME\n", 6);

  // if (!c2sBuf->Flush()) // blocks on write I/O
  //   dlog("c2sBuf->Flush() failed");
  proto.Flush();

  //flushWireBuffers();
}


const char* sockfile = "server.sock";


struct {
  int  fd;
  bool gotWelcomeMessage;
} conn;

enum ReadState {
  Frame,
  DawnWireHeader,
  DawnWireBody,
};

dawn_wire::ReservedTexture reservedTexture;


void runloop_main(int fd) {
  RunLoop* rl = EV_DEFAULT;
  FDSetNonBlock(fd);

  device = createWebGPUDevice().Release();
  init_dawn();

  ::memset(&conn, 0, sizeof(conn));
  conn.fd = fd;
  //c2sBuf->w = fd;

  proto.onFrame = []() {
    dlog("onFrame");
    // reserve a texture
    //struct ReservedTexture {
    //    WGPUTexture texture;
    //    uint32_t id;
    //    uint32_t generation;
    //    uint32_t deviceId;
    //    uint32_t deviceGeneration;
    //};
    if (reservedTexture.texture != nullptr)
      wireClient->ReclaimTextureReservation(reservedTexture);
    reservedTexture = wireClient->ReserveTexture(device);

    render_frame(reservedTexture.texture);
  };
  proto.onDawnBuffer = [](const void* data, size_t len) {
    dlog("onDawnBuffer len=%zu", len);
  };
  proto.start(rl, fd);

  ev_run(rl, 0);
  dlog("exit runloop");
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
    double t = ev_time();
    runloop_main(fd);
    close(fd);
    t = ev_time() - t;
    if (t < 1.0)
      sleep(1);
  }
  dlog("exit");
  return 0;
}
