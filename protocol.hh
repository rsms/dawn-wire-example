#pragma once
#include <unistd.h>
#include <assert.h>
#include <functional>
#include <limits>
#include <algorithm>
#include <dawn_wire/Wire.h>

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

struct DawnClientServerProtocol : public dawn_wire::CommandSerializer {
  Pipe<DAWNCMD_BUFSIZE + 8> _rbuf; // incoming data (extra space for pipe impl)
  Pipe<4096>                _wbuf; // outgoing data (in addition to _dawnout)

  RunLoop* _rl;
  ev_io    _io;
  size_t   _dawnCmdRLen = 0; // reamining nbytes to read as dawn command buffer

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

  // callbacks
  std::function<void()> onFrame;
  std::function<void(const char* data, size_t len)> onDawnBuffer;

  int fd() const { return _io.fd; }

  void start(RunLoop* rl, int fd);
  void stop();
  bool stopped() const { return _rl == nullptr; }

  bool writeFrame();
  bool writeDawnCommands(const char* src, size_t nbyte);

  // dawn_wire::CommandSerializer
  size_t GetMaximumAllocationSize() const override { return DAWNCMD_MAX; }
  void* GetCmdSpace(size_t size) override;
  bool Flush() override;


  // internal
  void beginFrame();
  inline void setNeedsWriteFlush() {
    if ((_io.events & EV_WRITE) == 0)
      setNeedsWriteFlush2();
  }
  void setNeedsWriteFlush2();
  void doIO(int revents);
  bool maybeFlushIncomingDawnCmd();
};
