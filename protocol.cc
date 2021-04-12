#include "protocol.hh"

#include <iostream>

#include <unistd.h> // pipe
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h> // F_GETFL, O_NONBLOCK etc


#define DLOG_PREFIX "[proto] "

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


static void DawnClientServerProtocol_doIO(RunLoop* rl, ev_io* w, int revents) {
  DawnClientServerProtocol* p = (DawnClientServerProtocol*)w->data;
  p->doIO(revents);
}

bool DawnClientServerProtocol::maybeFlushIncomingDawnCmd() {
  assert(_dawnCmdRLen > 0);
  assert(_dawnCmdRLen <= DAWNCMD_MAX);
  if (_rbuf.len() < _dawnCmdRLen) {
    return false;
  }

  // onDawnBuffer expects a contiguous memory segment
  const char* buf = _rbuf.takeContiguousRef(_dawnCmdRLen);
  if (buf == nullptr) {
    // copy into temporary buffer
    dlog("copy into temporary buffer");
    _rbuf.takeCopy(_dawncmdRead, _dawnCmdRLen);
    buf = _dawncmdRead;
  }
  onDawnBuffer(buf, _dawnCmdRLen);
  _dawnCmdRLen = 0;
  return true;
}

void DawnClientServerProtocol::doIO(int revents) {
  dlog("onConnIO %s %s",
    revents & EV_READ ? "EV_READ" : "",
    revents & EV_WRITE ? "EV_WRITE" : "");

  if (revents & EV_READ) {
    ssize_t n = _rbuf.addRead(_io.fd, _rbuf.cap()) > 0;
    if (n <= 0) {
      if (n < 0) {
        if (errno == EAGAIN)
          return;
        perror("read");
      }
      stop();
      return;
    }

    if (_dawnCmdRLen > 0) {
      maybeFlushIncomingDawnCmd();
    } else {
      if (_rbuf.len() > 0 && _rbuf._tail[0] == 'F') {
        _rbuf.takeDiscard(1);
        beginFrame();
      }

      if (_rbuf.len() >= 9 && _rbuf._tail[0] == 'D') {
        char buf[10];
        _rbuf.takeCopy(buf, 9);
        buf[9] = '\0';
        long l = strtol(&buf[1], NULL, 16);
        if (l < 1) {
          dlog("bad D message");
          stop();
          return;
        }
        _dawnCmdRLen = (size_t)l;
        maybeFlushIncomingDawnCmd();
      } else if (_rbuf.len() > 0) {
        dlog("unexpected message (first byte: '%c' 0x%02x)", _rbuf._tail[0], _rbuf._tail[0]);
        stop();
        return;
      }
    }
  }

  if (revents & EV_WRITE) {

    if (_dawncmd.flush) {
      dlog("_dawncmd.flush len=%u", _dawncmd.len);
      assert(_dawncmd.len > 0);
      ssize_t n = ::write(_io.fd, &_dawncmd.buf[_dawncmd.woffs], _dawncmd.len);
      if (n < 1) {
        if (n < 0 && errno != EAGAIN)
          perror("write");
        // not closing here; let EV_READ handle EOF and permanent errors
        return;
      }
      _dawncmd.len -= (uint32_t)n;
      if (_dawncmd.len == 0) {
        _dawncmd.flush = false;
        _dawncmd.woffs = 0;
      } else {
        _dawncmd.woffs += (uint32_t)n;
      }
      return;
    }

    size_t nbyte = _wbuf.len();
    if (nbyte > 0) {
      ssize_t z = _wbuf.takeWrite(_io.fd, nbyte);
      dlog("_wbuf.takeWrite(%zu) => %zd", nbyte, z);
      if (z < 0 && errno != EAGAIN) {
        perror("write");
        stop();
        return;
      }
    }

    // stop requesting EV_WRITE if there's nothing waiting to be written
    if (_wbuf.len() == 0) {
      ev_io_stop(_rl, &_io);
      ev_io_modify(&_io, _io.events & ~EV_WRITE);
      ev_io_start(_rl, &_io);
    }
  }
}

void DawnClientServerProtocol::start(RunLoop* rl, int fd) {
  _rbuf.reset();
  _wbuf.reset();
  _dawncmd.len = 0;
  _dawncmd.woffs = 0;
  _dawncmd.flush = false;
  _rl = rl;
  _io.data = (void*)this;
  ev_io_init(&_io, DawnClientServerProtocol_doIO, fd, EV_READ);
  ev_io_start(rl, &_io);
}

void DawnClientServerProtocol::stop() {
  if (_rl != nullptr) {
    ev_io_stop(_rl, &_io);
    _rl = nullptr;
  }
}

void DawnClientServerProtocol::setNeedsWriteFlush2() {
  if (_rl != nullptr) {
    ev_io_stop(_rl, &_io);
    ev_io_modify(&_io, _io.events | EV_WRITE);
    ev_io_start(_rl, &_io);
  }
}

bool DawnClientServerProtocol::writeFrame() {
  if (_wbuf.avail() < 1) {
    dlog("not enough buffer space for dawn command buffer");
    return false;
  }
  if (_wbuf.addCopy("F", 1) != 1)
    return false;
  setNeedsWriteFlush();
  return true;
}

bool DawnClientServerProtocol::writeDawnCommands(const char* src, size_t nbyte) {
  const size_t header_size = 9;
  size_t needbytes = nbyte + header_size;

  // proto buffer must be at least the size of the dawn command buffer + header
  // to be able to succeed at all.
  assert(_wbuf.cap() >= needbytes);

  // if there's no room, ask the caller to try again later
  if (_wbuf.avail() < needbytes) {
    dlog("not enough buffer space for dawn command buffer");
    return false;
  }

  // write header: "D" <HEXBYTE>{8}
  char buf[header_size + 1];
  assert(nbyte <= 0xFFFFFFFF);
  int d = snprintf(buf, sizeof(buf), "D%08x", (uint32_t)nbyte);
  assert(d == header_size);
  size_t n = _wbuf.addCopy(buf, header_size);
  assert(n == header_size);
  n = _wbuf.addCopy(src, nbyte);
  assert(n == nbyte);
  setNeedsWriteFlush();
  return true;
}

// -----------------------------------------------------------------------------------------------
// DawnClientServerProtocol, dawn_wire::CommandSerializer

// "D" <HEXBYTE>{8}
#define DAWNCMD_MSG_HEADER_SIZE 9


void DawnClientServerProtocol::beginFrame() {
  if (_dawncmd.flush) {
    // a new frame started before we had a chance to finish writing the last frame
    dlog("WARNING: new frame while still writing old frame; skipping this frame");
    return;
  }

  // allocate space for header: "D" <HEXBYTE>{8}
  _dawncmd.len = DAWNCMD_MSG_HEADER_SIZE;
  _dawncmd.woffs = 0;

  onFrame();
}

void* DawnClientServerProtocol::GetCmdSpace(size_t size) {
  assert(size <= sizeof(_dawncmd.buf));
  char* result = &_dawncmd.buf[_dawncmd.len];
  if (sizeof(_dawncmd.buf) - size < _dawncmd.len) {
    // not enough space; flush to free up space
    if (!Flush())
      return nullptr;
    return GetCmdSpace(size);
  }
  _dawncmd.len += size;
  return result;
}

bool DawnClientServerProtocol::Flush() {
  if (_dawncmd.len > DAWNCMD_MSG_HEADER_SIZE) {
    // write header (preallocated at buf[0])
    // use a temporary buffer for snprintf as it writes a trailing null byte.
    // TODO: use something else than snprintf which doesn't write a trailing null byte.
    char buf[DAWNCMD_MSG_HEADER_SIZE+1];
    snprintf(buf, sizeof(buf), "D%08x", _dawncmd.len - DAWNCMD_MSG_HEADER_SIZE);
    memcpy(_dawncmd.buf, buf, DAWNCMD_MSG_HEADER_SIZE);
    _dawncmd.flush = true;
    setNeedsWriteFlush();
  }
  return true;
}

// -----------------------------------------------------------------------------------------------
// IOBuffer

template <size_t Cap>
void IOBuffer<Cap>::reset() {
  _head = _storage;
  _tail = _storage;
}

template <size_t Cap>
ssize_t IOBuffer<Cap>::addRead(int fd, size_t nbyte) {
  const char* bufend = end();
  size_t nfree = avail();
  assert(bufend > _head /* is not end of buffer */);
  size_t nbyteMax = (size_t)((uintptr_t)bufend - (uintptr_t)_head);
  nbyte = MIN(nbyteMax, nbyte);
  ssize_t n = ::read(fd, _head, nbyte);
  if (n > 0) {
    // dlog("read %zd  '%c' 0x%02x", n, _head[0], _head[0]);
    assert(_head + n <= bufend);
    _head += n;
    if (_head == bufend)
      _head = _storage; // wrap around
    // fix up the tail pointer if an overflow occurred
    if (n > nfree)
      _tail = next(_head);
  }
  return n;
}

template <size_t Cap>
size_t IOBuffer<Cap>::addCopy(const char* src, size_t nbyte) {
  nbyte = MIN(avail(), nbyte);
  const char* bufend = end();
  size_t nread = 0;
  while (nread != nbyte) {
    assert(_head < bufend);
    size_t nbyteMax = (size_t)((uintptr_t)bufend - (uintptr_t)_head);
    size_t n = MIN(nbyteMax, nbyte - nread);
    memcpy(_head, src + nread, n);
    _head += n;
    nread += n;
    if (_head == bufend)
      _head = _storage; // wrap around
  }
  return nbyte;
}

template <size_t Cap>
void IOBuffer<Cap>::takeCopy(char* copyDst, size_t nbyte) {
  size_t len0 = len();
  assert(nbyte <= len0);
  const char* bufend = end();
  size_t nwritten = 0;
  while (nwritten < nbyte) {
    assert(bufend > _tail);
    size_t nbyteMax = (size_t)((uintptr_t)bufend - (uintptr_t)_tail);
    size_t n = MIN(nbyteMax, nbyte - nwritten);
    if (copyDst)
      memcpy(copyDst + nwritten, _tail, n);
    _tail += n;
    nwritten += n;
    if (_tail == bufend)
      _tail = _storage; // wrap around
  }
  assert(nwritten == nbyte);
  assert(nbyte + len() == len0 /* copied exactly nbyte */);
}

template <size_t Cap>
ssize_t IOBuffer<Cap>::takeWrite(int fd, size_t nbyte) {
  size_t len0 = len();
  assert(nbyte <= len0);
  const char* bufend = end();
  assert(bufend > _head);
  size_t nbyteMax = (size_t)((uintptr_t)bufend - (uintptr_t)_tail);
  nbyte = MIN(nbyteMax, nbyte);
  ssize_t n = ::write(fd, _tail, nbyte);
  if (n > 0) {
    assert(_tail + n <= bufend);
    _tail += n;
    if (_tail == bufend)
      _tail = _storage; // wrap around
    assert(n + len() == len0);
  }
  return n;
}

template <size_t Cap>
const char* IOBuffer<Cap>::takeContiguousRef(size_t nbyte) {
  const char* bufend = end();
  size_t contAvail = (size_t)((uintptr_t)bufend - (uintptr_t)_tail);
  if (_head >= _tail)
    contAvail = (size_t)((uintptr_t)_head - (uintptr_t)_tail);
  if (contAvail < nbyte)
    return nullptr;
  const char* p = _tail;
  if (nbyte == contAvail) {
    // wrap around
    _tail = _storage;
  } else {
    _tail += nbyte;
  }
  return p;
}

// // little unit test for IOBuffer
// __attribute__((constructor)) static void IOBufferTest() {
//   IOBuffer buf;
//   char* data = new char[buf.cap()];
//   // wrap-around
//   size_t chunk = buf.cap() / 5;
//   for (int i = 0; i < 6; i++) {
//     printf("\n");
//     if (buf._head >= buf._tail) {
//       dlog("avail(A): %zu", buf.avail());
//     } else {
//       dlog("avail(B): %zu", buf.avail());
//     }
//     dlog("addCopy %zu", chunk);
//     buf.addCopy(data, chunk);
//     if (buf._head >= buf._tail) {
//       dlog("avail(A): %zu", buf.avail());
//     } else {
//       dlog("avail(B): %zu", buf.avail());
//     }
//     buf.takeCopy(data, MIN(chunk, buf.len()));
//   }
//   delete[] data;
//   printf("IOBufferTest OK\n");
//   ::exit(0);
// }

