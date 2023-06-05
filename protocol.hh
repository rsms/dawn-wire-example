#pragma once
#include <unistd.h>
#include <assert.h>
#include <functional>
#include <limits>
#include <algorithm>

#include <dawn/webgpu_cpp.h>
#include <dawn/wire/Wire.h>
#include <dawn/wire/WireClient.h>

// DEBUG_TRACE_PROTOCOL: define to trace protocol I/O
// #define DEBUG_TRACE_PROTOCOL
#if defined(DEBUG_TRACE_PROTOCOL) && !defined(DEBUG_TRACE_PIPE)
  #define DEBUG_TRACE_PIPE
#endif
#include "pipe.hh"

// silence "mangled name of 'ev_set_allocator' will change in C++17"
_Pragma("GCC diagnostic push")
_Pragma("GCC diagnostic ignored \"-Wc++17-compat-mangling\"")
#include <ev.h>
_Pragma("GCC diagnostic pop")

typedef struct ev_loop RunLoop;

// dawn buffer sizes
#define DAWNCMD_MSG_HEADER_SIZE 9  /* "D" <HEXBYTE>{8} */
#define DAWNCMD_MAX             (4096*32)
#define DAWNCMD_BUFSIZE         (DAWNCMD_MAX + DAWNCMD_MSG_HEADER_SIZE)

struct DawnRemoteProtocol : public dawn::wire::CommandSerializer {
  struct FramebufferInfo {
    wgpu::TextureFormat textureFormat;
    wgpu::TextureUsage  textureUsage;
    uint32_t width, height; // pixels, not dp
    uint16_t dpscale; // 1dp = Npx (10x percent; 0% = 0, 100% = 1000, 250% = 2500 ...)
  };

  Pipe<DAWNCMD_BUFSIZE + 8> _rbuf; // incoming data (extra space for pipe impl)
  Pipe<4096>                _wbuf; // outgoing data (in addition to _dawnout)

  RunLoop* _rl;
  ev_io    _io;
  uint32_t _dawnCmdRLen = 0; // reamining nbytes to read as dawn command buffer

  // _dawnout is the dawn command buffer for outgoing Dawn command data
  struct {
    char     bufs[2][DAWNCMD_BUFSIZE];
    char*    writebuf = bufs[0]; // buffer used for GetCmdSpace
    uint32_t writelen = DAWNCMD_MSG_HEADER_SIZE; // length of writebuf
    char*    flushbuf = bufs[1]; // buffer being written to _io.fd
    uint32_t flushlen = 0; // length of flushbuf (>0 when flushing)
    uint32_t flushoffs = 0; // start offset of flushbuf
  } _dawnout;

  // _dawntmp is used for temporary storage of incoming dawn command buffers
  // in the case that they span across Pipe boundaries.
  char _dawntmp[DAWNCMD_MAX];

  // framebuffer info (only used by client)
  FramebufferInfo _fbinfo;

  // callbacks, client and server
  std::function<void(const char* data, size_t len)> onDawnBuffer;

  // callbacks, client only
  std::function<void()> onFrame; // server is ready for a new frame
  // onFramebufferInfo is called whenever the underlying framebuffer changes.
  // The argument provided is the same as returned by the fbinfo() method.
  std::function<void(const FramebufferInfo& fbinfo)> onFramebufferInfo;

  // callbacks, server only
  // onSwapchainReservation is called when the client has made a swapchain reservation.
  std::function<void(const dawn_wire::ReservedSwapChain&)> onSwapchainReservation;

  int fd() const { return _io.fd; }

  // client only
  const FramebufferInfo& fbinfo() const { return _fbinfo; }

  void start(RunLoop* rl, int fd);
  void stop();
  bool stopped() const { return _rl == nullptr; }

  bool sendFrameSignal();
  bool sendFramebufferInfo(const FramebufferInfo& info);
  bool sendReservation(const dawn_wire::ReservedSwapChain& scr);
  // bool sendDawnCommands(const char* src, size_t nbyte);

  // dawn_wire::CommandSerializer
  size_t GetMaximumAllocationSize() const override { return DAWNCMD_MAX; }
  void* GetCmdSpace(size_t size) override;
  bool Flush() override;


  // internal
  inline void setNeedsWriteFlush() {
    if ((_io.events & EV_WRITE) == 0)
      setNeedsWriteFlush2();
  }
  void setNeedsWriteFlush2();
  void doIO(int revents);
  bool readMsg();
  bool maybeReadIncomingDawnCmd();
};
