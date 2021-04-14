#include "pipe.hh"
#include "debug.hh"
#include <stdio.h>

#define DLOG_PREFIX "[pipe] "

#ifdef DEBUG
  #define dlog(format, ...) ({ \
    fprintf(stderr, DLOG_PREFIX format " \e[2m(%s %d)\e[0m\n", \
      ##__VA_ARGS__, __FUNCTION__, __LINE__); \
    fflush(stderr); \
  })
#else
  #define dlog(...) do{}while(0)
#endif


void _PipeTrace(const char* name, const char* prefix, const char* data, size_t datalen) {
  char* buf = (char*)malloc(datalen*5);
  ssize_t n = data != nullptr ? debugFmtBytes(buf, datalen*5, data, datalen) : 0;
  if (n != -1) {
    if (n > 80) {
      fprintf(stderr, "%s  %s  %zu\n\"%s\"\n", name, prefix, datalen, buf);
    } else {
      fprintf(stderr, "%s  %s  %zu  \"%s\"\n", name, prefix, datalen, buf);
    }
  }
  free(buf);
}


#if 0

// little unit test for Pipe
__attribute__((constructor)) static void PipeTest() {
  Pipe<32> pipe;
  char rbuf[pipe.cap()*2];
  #define DUMPSTATE \
    dlog(">> w %zu  r %zu  len %zu  avail %zu", pipe._w, pipe._r, pipe.len(), pipe.avail())
  size_t r;

  // Note: tests expect first chunk to be at least 2 bytes long
  std::vector<const char*> sampleData{"hello", "worlds", "internetofshit", "a"};

  { // write something that's too large to fit
    assert(pipe.len() == 0); // should be empty
    assert(pipe.avail() == pipe.cap()); // should be empty
    r = pipe.write(rbuf, pipe.cap() * 2);
    assert(r == pipe.cap()); // should only write as much as fits
    assert(pipe.avail() == 0); // should be full
    assert(pipe.len() == pipe.cap()); // should be full
  }

  { // read all
    size_t len = pipe.len();
    r = pipe.read(rbuf, len);
    assert(r == len);
    assert(pipe.len() == 0); // should be empty
    assert(pipe.avail() == pipe.cap()); // should be empty
  }

  // write, read, write, read ...
  for (auto& chunk : sampleData) {
    size_t chunklen = strlen(chunk);

    dlog("write \"%s\" %zu", chunk, chunklen);
    r = pipe.write(chunk, chunklen);
    assert(r == chunklen);

    dlog("read %zu ...", chunklen);
    r = pipe.read(rbuf, chunklen);
    assert(r == chunklen);
    assert(memcmp(rbuf, chunk, chunklen) == 0);
  }

  // advance pipe so that we write with an overlap next
  pipe.clear(); // resets _w & _r to 0
  assert(pipe.len() == 0); // should be empty
  pipe.write(rbuf, pipe.cap() - 1);
  assert(pipe.len() == pipe.cap() - 1);
  assert(pipe.avail() == 1);
  pipe.read(rbuf, pipe.cap() - 1);
  assert(pipe.len() == 0); // should be empty
  DUMPSTATE;
  fwrite("\n", 1, 1, stderr);

  // write, write ...  First chunk will wrap around
  for (auto& chunk : sampleData) {
    size_t chunklen = strlen(chunk);
    dlog("write \"%s\" %zu", chunk, chunklen);
    r = pipe.write(chunk, chunklen);
    assert(r == chunklen);
  }

  // read, read ...
  for (auto& chunk : sampleData) {
    size_t chunklen = strlen(chunk);
    dlog("read \"%s\" %zu", chunk, chunklen);
    r = pipe.read(rbuf, chunklen);
    assert(r == chunklen);
    assert(memcmp(rbuf, chunk, chunklen) == 0);
  }

  // takeRef success
  pipe.clear();
  const char* chunk = "hello world";
  size_t chunklen = strlen(chunk);
  r = pipe.write(chunk, chunklen);
  assert(r == chunklen);
  const char* ref = pipe.takeRef(chunklen);
  assert(ref != nullptr);
  assert(memcmp(ref, chunk, chunklen) == 0);

  // takeRef failure (overlaps)
  pipe.clear();
  // fill up so that first byte is at R1, Nth bytes at R2
  pipe.write(rbuf, pipe.cap() - chunklen/2);
  pipe.read(rbuf, pipe.cap() - chunklen/2);
  r = pipe.write(chunk, chunklen); // write with overlap
  assert(r == chunklen);
  ref = pipe.takeRef(chunklen);
  assert(ref == nullptr); // should fail
  // however a copy read works
  r = pipe.read(rbuf, chunklen);
  assert(r == chunklen);
  assert(memcmp(rbuf, chunk, chunklen) == 0);

  { // file descriptors
    fwrite("\n", 1, 1, stderr);
    dlog("FD test with test input \"%s\"[%zu]", chunk, chunklen);
    int fds[2]; // 0=read, 1=write
    int ok = ::pipe(fds);
    assert(ok == 0);
    pipe.clear();
    // add some data to the pipe
    r = pipe.write(chunk, chunklen);
    assert(r == chunklen);
    assert(pipe.len() == chunklen);
    // read from Pipe and write to fd
    ssize_t sum = 0;
    while (sum < (ssize_t)chunklen) {
      ssize_t n = pipe.writeToFD(fds[1], chunklen);
      dlog("writeToFD => %zd", n);
      if (n < 0)
        perror("writeToFD");
      assert(n > 0);
      sum += n;
    }
    // pipe should now be empty (its data moved to the OSes pipe; fds)
    assert(pipe.len() == 0);
    // read from fd and write to Pipe
    sum = 0;
    while (sum < (ssize_t)chunklen) {
      ssize_t n = pipe.readFromFD(fds[0], chunklen);
      dlog("readFromFD => %zd", n);
      if (n < 0)
        perror("readFromFD");
      assert(n > 0);
      sum += n;
    }
    // pipe should now be populated with the original message
    dlog("pipe.len() %zu", pipe.len());
    assert(pipe.len() == chunklen);
    assert(memcmp(rbuf, chunk, chunklen) == 0);
    ::close(fds[1]);
    ::close(fds[0]);
  }

  printf("---- %s OK\n", __FUNCTION__);
  // free(data);
  ::exit(0);
  #undef DUMPSTATE
}

#endif /* unit test */
