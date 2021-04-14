#include "protocol.hh"
#include "debug.hh"

#include <cstdio>
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


// protocol messages
//
// message        = metaMsg | frameMsg | dawncmdMsg
// frameInfoMsg   = "I" <TODO DATA>
// frameSignalMsg = "F"
// reservationMsg = "R" <TODO DATA>
// dawncmdMsg     = "D" size
// size           = <uint32 in big-endian order>
//
#define MSGT_FB_INFO       'I' /* Framebuffer info */
#define MSGT_FRAME_SIGNAL  'F' /* Frame signal */
#define MSGT_RESERVATION   'R' /* Device and Swapchain reservations */
#define MSGT_DAWNCMD       'D' /* Dawn command buffer */

// FB_INFO_SIZE is the number of bytes occupied by encoded framebuffer info
#define FB_INFO_SIZE sizeof(DawnRemoteProtocol::FramebufferInfo)

#define RESERVATION_SIZE (sizeof(dawn_wire::ReservedDevice) + sizeof(dawn_wire::ReservedSwapChain))


// encodeDawnCmdHeader writes a MSGT_DAWNCMD header of DAWNCMD_MSG_HEADER_SIZE bytes to dst.
static void encodeDawnCmdHeader(char* dst, uint32_t dawncmdlen) {
  dst[0] = MSGT_DAWNCMD;
  *((uint32_t*)&dst[1]) = htonl(dawncmdlen);
}

static void decodeDawnCmdHeader(const char* src, uint32_t* dawncmdlen) {
  assert(src[0] == MSGT_DAWNCMD);
  *dawncmdlen = ntohl(*((uint32_t*)&src[1]));
}

static void decodeFramebufferInfo(const char* src, DawnRemoteProtocol::FramebufferInfo* fbinfo) {
  assert(src[0] == MSGT_FB_INFO);
  *fbinfo = *((DawnRemoteProtocol::FramebufferInfo*)&src[1]); // FIXME
}

static void encodeFramebufferInfo(char* dst, const DawnRemoteProtocol::FramebufferInfo& info) {
  dst[0] = MSGT_FB_INFO;
  *((DawnRemoteProtocol::FramebufferInfo*)&dst[1]) = info; // FIXME
}

static void encodeReservation(char* dst, const dawn_wire::ReservedSwapChain& scr) {
  dst[0] = MSGT_RESERVATION;
  *((dawn_wire::ReservedSwapChain*)&dst[1]) = scr; // FIXME
}

static void decodeReservation(const char* src, dawn_wire::ReservedSwapChain* scr) {
  assert(src[0] == MSGT_RESERVATION);
  *scr = *((dawn_wire::ReservedSwapChain*)&src[1]); // FIXME
}



bool DawnRemoteProtocol::sendFrameSignal() {
  if (_wbuf.avail() < 1) {
    trace("not enough buffer space in _wbuf");
    return false;
  }
  if (_wbuf.writec(MSGT_FRAME_SIGNAL) != 1)
    return false;
  setNeedsWriteFlush();
  return true;
}

bool DawnRemoteProtocol::sendFramebufferInfo(const FramebufferInfo& info) {
  char tmp[FB_INFO_SIZE+1];
  if (_wbuf.avail() < sizeof(tmp)) {
    trace("not enough buffer space in _wbuf");
    return false;
  }
  encodeFramebufferInfo(tmp, info);
  _wbuf.write(tmp, sizeof(tmp));
  setNeedsWriteFlush();
  return true;
}

bool DawnRemoteProtocol::sendReservation(const dawn_wire::ReservedSwapChain& scr) {
  char tmp[RESERVATION_SIZE+1];
  if (_wbuf.avail() < sizeof(tmp)) {
    trace("not enough buffer space in _wbuf");
    return false;
  }
  encodeReservation(tmp, scr);
  _wbuf.write(tmp, sizeof(tmp));
  setNeedsWriteFlush();
  return true;
}



bool DawnRemoteProtocol::maybeReadIncomingDawnCmd() {
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

// readMsg reads a protocol message from the read buffer (_rbuf)
bool DawnRemoteProtocol::readMsg() {
  char tmp[MAX(MAX(DAWNCMD_MSG_HEADER_SIZE, FB_INFO_SIZE), RESERVATION_SIZE) + 1];
  while (_rbuf.len() > 0) {
    switch (_rbuf.at(0)) {

    case MSGT_FB_INFO: {
      trace("MSGT_FB_INFO");
      _rbuf.read(tmp, FB_INFO_SIZE + 1);
      decodeFramebufferInfo(tmp, &_fbinfo);
      onFramebufferInfo(_fbinfo);
      break;
    }

    case MSGT_RESERVATION: {
      trace("MSGT_RESERVATION");
      _rbuf.read(tmp, RESERVATION_SIZE + 1);
      dawn_wire::ReservedSwapChain scr;
      decodeReservation(tmp, &scr);
      onSwapchainReservation(scr);
      break;
    }

    case MSGT_FRAME_SIGNAL: {
      trace("MSGT_FRAME_SIGNAL");
      _rbuf.discard(1);
      if (_dawnout.flushlen == 0) {
        onFrame(); // user callback
      } else {
        // a new frame started before we had a chance to finish writing the last frame
        dlog("WARNING: new frame while still writing old frame; skipping this frame");
      }
      break;
    }

    case MSGT_DAWNCMD: {
      trace("MSGT_DAWNCMD _rbuf.len() = %zu, _rbuf[0] = 0x%02X", _rbuf.len(), _rbuf.at(0));
      if (_rbuf.len() >= DAWNCMD_MSG_HEADER_SIZE) {
        _rbuf.read(tmp, DAWNCMD_MSG_HEADER_SIZE);
        decodeDawnCmdHeader(tmp, &_dawnCmdRLen);
        trace("start reading dawn command buffer of size %u", _dawnCmdRLen);
        maybeReadIncomingDawnCmd();
      }
      break;
    }

    default: {
      // unexpected/corrupt message data
      char c = _rbuf.at(0);
      errlog("unexpected message (first byte: '%c' 0x%02x, rbuf.len(): %zu)", c, c, _rbuf.len());
      trace("closing connection");
      stop();
      return false;
    }
    } // switch
  } // while

  return true;
}

static void DawnRemoteProtocol_doIO(RunLoop* rl, ev_io* w, int revents) {
  DawnRemoteProtocol* p = (DawnRemoteProtocol*)w->data;
  p->doIO(revents);
}

void DawnRemoteProtocol::doIO(int revents) {
  // dlog("onConnIO %s %s",
  //   revents & EV_READ ? "EV_READ" : "",
  //   revents & EV_WRITE ? "EV_WRITE" : "");

  if (revents & EV_READ) {
    // read into _rbuf
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
      trace("maybeReadIncomingDawnCmd");
      maybeReadIncomingDawnCmd();
    } else {
      if (!readMsg())
        return;
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
        assert(_dawnout.flushoffs < _dawnout.flushlen);
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

void DawnRemoteProtocol::start(RunLoop* rl, int fd) {
  trace("START");
  _rbuf.clear();
  _wbuf.clear();
  #ifdef DEBUG
  _rbuf._debugname = "rbuf";
  _wbuf._debugname = "wbuf";
  #endif

  _rl = rl;
  _io.data = (void*)this;
  ev_io_init(&_io, DawnRemoteProtocol_doIO, fd, EV_READ);
  ev_io_start(rl, &_io);
}

void DawnRemoteProtocol::stop() {
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

void DawnRemoteProtocol::setNeedsWriteFlush2() {
  if (_rl != nullptr) {
    ev_io_stop(_rl, &_io);
    ev_io_modify(&_io, _io.events | EV_WRITE);
    ev_io_start(_rl, &_io);
  }
}

void* DawnRemoteProtocol::GetCmdSpace(size_t size) {
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

bool DawnRemoteProtocol::Flush() {
  trace("flush dawn command data %u", _dawnout.writelen);
  assert(_dawnout.flushlen == 0 /* is done flushing previous buffer */);
  if (_dawnout.writelen > DAWNCMD_MSG_HEADER_SIZE) {
    // write header (preallocated at writebuf[0..DAWNCMD_MSG_HEADER_SIZE])
    encodeDawnCmdHeader(_dawnout.writebuf, _dawnout.writelen - DAWNCMD_MSG_HEADER_SIZE);

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

// bool DawnRemoteProtocol::sendDawnCommands(const char* src, size_t nbyte) {
//   size_t needbytes = nbyte + DAWNCMD_MSG_HEADER_SIZE;
//   // proto buffer must be at least the size of the dawn command buffer + header
//   // to be able to succeed at all.
//   assert(_wbuf.cap() >= needbytes);
//   // if there's no room, ask the caller to try again later
//   if (_wbuf.avail() < needbytes) {
//     trace("not enough buffer space for dawn command buffer");
//     return false;
//   }
//   // write header: "D" <HEXBYTE>{8}
//   assert(nbyte <= 0xFFFFFFFF);
//   char buf[DAWNCMD_MSG_HEADER_SIZE];
//   encodeDawnCmdHeader(buf, (uint32_t)nbyte);
//   _wbuf.write(buf, DAWNCMD_MSG_HEADER_SIZE);
//   // not checking result from _wbuf.write as we already checked _wbuf.avail()
//   size_t n = _wbuf.write(src, nbyte);
//   assert(n == nbyte);
//   setNeedsWriteFlush();
//   return true;
// }
