#pragma once
#include <unistd.h>
#include <assert.h>
#include <functional>
#include <dawn_wire/Wire.h>

// silence "mangled name of 'ev_set_allocator' will change in C++17"
_Pragma("GCC diagnostic push")
_Pragma("GCC diagnostic ignored \"-Wc++17-compat-mangling\"")
#include <ev.h>
_Pragma("GCC diagnostic pop")

typedef struct ev_loop RunLoop;

// IOBuffer is a circular read-write buffer
template <size_t Capacity>
struct IOBuffer {
  char   _storage[Capacity]; // = sizeof() = cap() = max possible chunk size
  char*  _head = _storage;
  char*  _tail = _storage;

  void reset();

  size_t cap() const { return Capacity - 1; }
  size_t len() const { return cap() - avail(); }
  size_t avail() const {
    if (_head >= _tail)
      return cap() - (size_t)((uintptr_t)_head - (uintptr_t)_tail);
    return (size_t)((uintptr_t)_tail - (uintptr_t)_head);
  }

  const char* end() const { return &_storage[cap()]; }
  char* next(const char* p) {
    assert(p >= &_storage[0] && p < end());
    return &_storage[(++p - &_storage[0]) % cap()];
  }

  // addCopy adds up to nbyte bytes from src to the buffer.
  // returns number of bytes actually copied which is MIN(avail(), nbyte)
  size_t addCopy(const char* src, size_t nbyte);

  // addRead adds up to nbyte bytes to the buffer by read()-ing from fd
  ssize_t addRead(int fd, size_t nbyte);

  // takeCopy removes the nbyte oldest bytes, optionally copying them to copyDst.
  // If copyDst is null, bytes are simply freed up and no copying occurs.
  // nbyte must be less or equal to len()
  void takeCopy(char* copyDst, size_t nbyte);
  void takeDiscard(size_t nbyte) { takeCopy(nullptr, nbyte); }

  // takeWrite writes up to nbyte data in the buffer to fd
  // nbyte must be less or equal to len()
  // Returns the value of the write() call (-1: error, 0: EOF, >0: nbyte written)
  ssize_t takeWrite(int fd, size_t nbyte);

  // takeContiguousRef removes nbyte and returns a pointer to the removed bytes,
  // if and only if the next nbytes are contiguous, i.e. does not span across the
  // underlying ring buffer's head & tail.
  // Returns nullptr on failure.
  // The returned memory is only valid until the next call to an add*() function.
  const char* takeContiguousRef(size_t nbyte);
};

struct DawnClientServerProtocol : public dawn_wire::CommandSerializer {
  static const size_t DAWNCMD_MAX = 4096*32;

  IOBuffer<DAWNCMD_MAX+10> _rbuf;
  IOBuffer<4096>           _wbuf;

  RunLoop* _rl;
  ev_io    _io;
  size_t   _dawnCmdRLen = 0; // reamining nbytes to read as dawn command buffer

  // _dawncmd is the dawn command buffer for outgoing Dawn command data
  struct {
    char     buf[DAWNCMD_MAX];
    uint32_t len = 0;
    uint32_t woffs = 0; // write offset (only used while flush=true)
    bool     flush = false; // true when ready to be written
  } _dawncmd;

  // _dawncmdRead is used for temporary storage of incoming dawn command buffers
  // in the case that they span across IOBuffer boundaries.
  char _dawncmdRead[DAWNCMD_MAX];

  // callbacks
  std::function<void()> onFrame;
  std::function<void(const char* data, size_t len)> onDawnBuffer;

  int fd() const { return _io.fd; }

  void start(RunLoop* rl, int fd);
  void stop();

  bool writeFrame();
  bool writeDawnCommands(const char* src, size_t nbyte);

  // dawn_wire::CommandSerializer
  size_t GetMaximumAllocationSize() const override { return sizeof(_dawncmd.buf); }
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
