#include "protocol.hh"
#include "debug.hh"

#include <errno.h>
#include <unistd.h> // pipe
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h> // F_GETFL, O_NONBLOCK etc
#include <ctype.h> // isprint


#define DLOG_PREFIX "[proto] "

#if defined(DEBUG)
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

#if defined(DEBUG_TRACE_PROTOCOL)
  #define trace(format, ...) ({ \
    fprintf(stderr, "\e[1;34m[proto trace]\e[0m " format " \e[2m(%s %d)\e[0m\n", \
      ##__VA_ARGS__, __FUNCTION__, __LINE__); \
    fflush(stderr); \
  })
#else
  #define trace(...) do{}while(0)
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
  if (_rbuf.len() < _dawnCmdRLen)
    return false;

  // onDawnBuffer expects a contiguous memory segment; attempt to simply reference
  // the data in rbuf. takeRef returns null if the data is not available as a contiguous
  // segement, in which case we resort to copying it into a temporary buffer.
  const char* buf = _rbuf.takeRef(_dawnCmdRLen);
  if (buf == nullptr) {
    // copy into temporary buffer
    trace("copy into temporary buffer _dawntmp");
    _rbuf.read(_dawntmp, _dawnCmdRLen);
    buf = _dawntmp;
  }
  onDawnBuffer(buf, _dawnCmdRLen);
  _dawnCmdRLen = 0;
  return true;
}

void DawnClientServerProtocol::doIO(int revents) {
  // dlog("onConnIO %s %s",
  //   revents & EV_READ ? "EV_READ" : "",
  //   revents & EV_WRITE ? "EV_WRITE" : "");

  if (revents & EV_READ) {
    ssize_t n = _rbuf.readFromFD(_io.fd, _rbuf.cap()) > 0;
    if (n <= 0) {
      if (n < 0) {
        if (errno == EAGAIN)
          return;
        perror("read");
      }
      trace("EOF");
      stop();
      return;
    }
    trace("read %zd bytes into _rbuf; _rbuf.len() = %zu", n, _rbuf.len());

    if (_dawnCmdRLen > 0) {
      trace("maybeFlushIncomingDawnCmd");
      maybeFlushIncomingDawnCmd();
    } else {
      if (_rbuf.len() > 0 && _rbuf.at(0) == 'F') {
        trace("FRAME");
        _rbuf.discard(1);
        beginFrame();
      }

      trace("_rbuf.len() = %zu, _rbuf[0] = 0x%02X", _rbuf.len(), _rbuf.at(0));

      if (_rbuf.len() >= 9 && _rbuf.at(0) == 'D') {
        char buf[10];
        _rbuf.read(buf, 9);
        buf[9] = '\0';
        long l = strtol(&buf[1], NULL, 16);
        if (l < 1) {
          trace("bad D message");
          stop();
          return;
        }
        _dawnCmdRLen = (size_t)l;
        trace("start reading dawn command buffer of size %zu", _dawnCmdRLen);
        maybeFlushIncomingDawnCmd();
      } else if (_rbuf.len() > 0) {
        errlog("unexpected message (first byte: '%c' 0x%02x, rbuf.len(): %zu)",
          _rbuf.at(0), _rbuf.at(0), _rbuf.len());
        trace("closing connection");
        stop();
        return;
      }
    }
  }

  if (revents & EV_WRITE) {

    // if we are flushing Dawn command data, do that before draining _wbuf
    if (_dawnout.flushlen != 0) {
      assert(_dawnout.flushlen > _dawnout.flushoffs);
      uint32_t len = _dawnout.flushlen - _dawnout.flushoffs;
      trace("_dawnout flush [offs=%u, len=%u]", _dawnout.flushoffs, len);
      ssize_t n = ::write(_io.fd, &_dawnout.flushbuf[_dawnout.flushoffs], len);
      if (n < 1) {
        if (n < 0 && errno != EAGAIN)
          perror("write");
        return;
      }
      _dawnout.flushoffs += (uint32_t)n;
      if (_dawnout.flushlen == _dawnout.flushoffs) {
        trace("_dawnout flush done");
        _dawnout.flushlen = 0;
      } else {
        // we weren't able to write all of _dawnout.flushbuf; return and wait for more EV_WRITE
        trace("_dawnout flush more");
        assert(_dawnout.flushlen < _dawnout.flushoffs);
        return;
      }
    }

    // drain _wbuf
    size_t nbyte = _wbuf.len();
    if (nbyte > 0) {
      ssize_t z = _wbuf.writeToFD(_io.fd, nbyte);
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
  trace("START");
  _rbuf.clear();
  _wbuf.clear();
  #ifdef DEBUG
  _rbuf._debugname = "rbuf";
  _wbuf._debugname = "wbuf";
  #endif

  _rl = rl;
  _io.data = (void*)this;
  ev_io_init(&_io, DawnClientServerProtocol_doIO, fd, EV_READ);
  ev_io_start(rl, &_io);
}

void DawnClientServerProtocol::stop() {
  trace("STOP");
  // reset _dawnout
  _dawnout.writelen = DAWNCMD_MSG_HEADER_SIZE;
  _dawnout.flushlen = 0;
  // unsubscribe from IO events
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
    trace("not enough buffer space for dawn command buffer");
    return false;
  }
  if (_wbuf.write("F", 1) != 1)
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
    trace("not enough buffer space for dawn command buffer");
    return false;
  }

  // write header: "D" <HEXBYTE>{8}
  char buf[header_size + 1];
  assert(nbyte <= 0xFFFFFFFF);
  int d = snprintf(buf, sizeof(buf), "D%08x", (uint32_t)nbyte);
  assert(d == header_size);
  size_t n = _wbuf.write(buf, header_size);
  assert(n == header_size);
  n = _wbuf.write(src, nbyte);
  assert(n == nbyte);
  setNeedsWriteFlush();
  return true;
}

void DawnClientServerProtocol::beginFrame() {
  if (_dawnout.flushlen != 0) {
    // a new frame started before we had a chance to finish writing the last frame
    dlog("WARNING: new frame while still writing old frame; skipping this frame");
    return;
  }
  onFrame(); // user callback
}

void* DawnClientServerProtocol::GetCmdSpace(size_t size) {
  trace("GetCmdSpace %zu", size);
  assert(size <= DAWNCMD_MAX);
  if (sizeof(_dawnout.writebuf) - size < _dawnout.writelen) {
    dlog("GetCmdSpace FAILED (not enough space)");
    return nullptr; // not enough space
  }
  char* result = &_dawnout.writebuf[_dawnout.writelen];
  _dawnout.writelen += size;
  return result;
}

bool DawnClientServerProtocol::Flush() {
  trace("flush dawn command data %u", _dawnout.writelen);
  assert(_dawnout.flushlen == 0 /* is done flushing previous buffer */);
  if (_dawnout.writelen > DAWNCMD_MSG_HEADER_SIZE) {
    // write header (preallocated at writebuf[0])
    // use a temporary buffer for snprintf as it writes a trailing null byte.
    // TODO: use something else than snprintf which doesn't write a trailing null byte.
    char tmp[DAWNCMD_MSG_HEADER_SIZE+1];
    snprintf(tmp, sizeof(tmp), "D%08x", _dawnout.writelen - DAWNCMD_MSG_HEADER_SIZE);
    memcpy(_dawnout.writebuf, tmp, DAWNCMD_MSG_HEADER_SIZE);

    #ifdef DEBUG_TRACE_PROTOCOL
    { // log buffer
      char* buf = (char*)malloc(_dawnout.writelen*5);
      ssize_t n = debugFmtBytes(buf, _dawnout.writelen*5, _dawnout.writebuf, _dawnout.writelen);
      if (n != -1)
        trace("data to be sent out: %u\n\"%s\"", _dawnout.writelen, buf);
      free(buf);
    }
    #endif /* DEBUG_TRACE_PROTOCOL */

    // swap buffers
    char* buf1 = _dawnout.flushbuf;
    _dawnout.flushbuf = _dawnout.writebuf;
    _dawnout.writebuf = buf1;

    // setup flush state
    _dawnout.flushlen = _dawnout.writelen;
    _dawnout.flushoffs = 0;
    setNeedsWriteFlush();

    // reset write
    _dawnout.writelen = DAWNCMD_MSG_HEADER_SIZE;
  } else {
    assert(_dawnout.writelen == DAWNCMD_MSG_HEADER_SIZE);
  }
  return true;
}
