#pragma once
#include <limits>
#include <algorithm>
#include <cstring>
#include <vector>
#include <unistd.h> // read, write, close


// DEBUG_TRACE_PIPE: define to enable verbose tracing of input and output data
// #define DEBUG_TRACE_PIPE

#ifdef DEBUG_TRACE_PIPE
  void _PipeTrace(const char* name, const char* msg, const char* data, size_t datalen);
  #ifdef DEBUG
    #define PipeTrace(msg, data, datalen) _PipeTrace(_debugname, msg, data, datalen)
  #else
    #define PipeTrace(msg, data, datalen) _PipeTrace("iobuf", msg, data, datalen)
  #endif
#else
#define PipeTrace(...) do{}while(0)
#endif


// Pipe is a circular read-write buffer.
// It works like this:
//
// initial:       storage: 0 1 2 3 4 5 6 7
// len: 0                  |
//                        w r
//
// write 5 bytes: storage: 0 1 2 3 4 5 6 7
// len: 5                  |         |
//                         r         w
//
// read 2 bytes:  storage: 0 1 2 3 4 5 6 7
// len: 3                      |     |
//                             r     w
//
// write 4 bytes: storage: 0 1 2 3 4 5 6 7
// len: 7                    | |
//                           w r
//
template <size_t Size>
struct Pipe {
  // the len function assumes Size < MAX_SIZE_T/2
  static_assert(Size < std::numeric_limits<size_t>::max()/2, "Size < MAX_SIZE_T/2");

  char   _storage[Size];
  size_t _w = 0; // storage write offset
  size_t _r = 0; // storage read offset

  #ifdef DEBUG
  const char* _debugname = "buf";
  #endif

  constexpr size_t cap() const { return Size - 1; }
  size_t len() const { return (Size - _r + _w) % Size; }
  size_t avail() const { return (Size - 1 - _w + _r) % Size; }

  // add data to the beginning of the pipe
  size_t  write(const char* src, size_t nbyte); // copy <=nbyte of dst into the pipe
  ssize_t readFromFD(int fd, size_t nbyte);     // read <=nbyte from file (-1 on error)

  // take data out of the end of the pipe
  size_t  read(char* dst, size_t nbyte);   // copy <=nbyte of data to dst
  size_t  discard(size_t nbyte);           // read & discard
  ssize_t writeToFD(int fd, size_t nbyte); // write <=nbyte to file (-1 on error)

  // takeRef removes nbyte and returns a pointer to the removed bytes,
  // if and only if the next nbytes are contiguous, i.e. does not span across the
  // underlying ring buffer's head & tail. Returns nullptr on failure.
  // The returned memory is only valid until the next call to write() or clear().
  const char* takeRef(size_t nbyte);

  inline char at(size_t index) const { return _storage[_r]; }

  // clear drains the pipe by discarding any data waiting to be read
  void clear() { _w = 0; _r = 0; }
};

template <size_t Size>
size_t Pipe<Size>::write(const char* data, size_t nbyte) {
  nbyte = std::min(nbyte, avail());
  PipeTrace("write", data, nbyte);
  size_t chunkend = std::min(nbyte, Size - _w);
  memcpy(_storage + _w, data, chunkend);
  memcpy(_storage, data + chunkend, nbyte - chunkend);
  _w = (_w + nbyte) % Size;
  return nbyte;
}

template <size_t Size>
ssize_t Pipe<Size>::readFromFD(int fd, size_t nbyte) {
  nbyte = std::min(nbyte, avail());
  size_t chunkend = std::min(nbyte, Size - _w);
  ssize_t total = 0;
  if (chunkend > 0) {
    total = ::read(fd, _storage + _w, chunkend);
    PipeTrace("readFromFD", _storage + _w, (size_t)(total < 0 ? 0 : total));
    if (total < (ssize_t)chunkend) {
      // short read
      if (total > -1)
        goto end;
      return total;
    }
  }
  if (nbyte > chunkend) {
    ssize_t n = ::read(fd, _storage, nbyte - chunkend);
    PipeTrace("readFromFD", _storage, (size_t)(n < 0 ? 0 : n));
    if (n < 0)
      return n;
    total += n;
  }
 end:
  _w = (_w + (size_t)total) % Size;
  return total;
}

template <size_t Size>
size_t Pipe<Size>::read(char* data, size_t nbyte) {
  nbyte = std::min(nbyte, len());
  size_t chunkend = std::min(nbyte, Size - _r);
  memcpy(data, _storage + _r, chunkend);
  if (chunkend > 0)
    PipeTrace("read (1)", data, chunkend);
  memcpy(data + chunkend, _storage, nbyte - chunkend);
  if (nbyte - chunkend > 0)
    PipeTrace("read (2)", data + chunkend, nbyte - chunkend);
  _r = (_r + nbyte) % Size;
  return nbyte;
}

template <size_t Size>
ssize_t Pipe<Size>::writeToFD(int fd, size_t nbyte) {
  nbyte = std::min(nbyte, len());
  size_t chunkend = std::min(nbyte, Size - _r);
  ssize_t total = 0;
  if (chunkend > 0) {
    total = ::write(fd, _storage + _r, chunkend);
    PipeTrace("writeToFD", _storage + _r, (size_t)(total < 0 ? 0 : total));
    if (total < (ssize_t)chunkend) {
      // short write
      if (total > -1)
        goto end;
      return total;
    }
  }
  if (nbyte > chunkend) {
    ssize_t n = ::write(fd, _storage, nbyte - chunkend);
    PipeTrace("writeToFD", _storage, (size_t)(n < 0 ? 0 : n));
    if (n < 0)
      return n;
    total += n;
  }
 end:
  _r = (_r + nbyte) % Size;
  return total;
}

template <size_t Size>
size_t Pipe<Size>::discard(size_t nbyte) {
  nbyte = std::min(nbyte, len());
  PipeTrace("discard", NULL, nbyte);
  _r = (_r + nbyte) % Size;
  return nbyte;
}

template <size_t Size>
const char* Pipe<Size>::takeRef(size_t nbyte) {
  // Either w is ahead of e in memory ...
  //   0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15
  //      W2   |        R1        |    W1      R=read-from, W=write-to
  //           r                  w
  // ... or r is ahead of w in memory ...
  //   0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15
  //      R2   |        W1        |    R1
  //           w                  r
  // In either case we can only return a reference to R1.
  nbyte = std::min(nbyte, len());
  size_t chunkend = std::min(nbyte, Size - _r);
  const char* p = nullptr;
  if (chunkend >= nbyte) {
    PipeTrace("takeRef", _storage + _r, nbyte);
    p = _storage + _r;
    _r = (_r + nbyte) % Size;
  } else {
    PipeTrace("takeRef", NULL, 0);
  }
  return p;
}
