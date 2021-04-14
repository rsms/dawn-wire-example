#include "debug.hh"
#include <cctype> // isprint, isspace
#include <stdio.h>

ssize_t debugFmtBytes(char* dst, size_t dstsize, const char* data, size_t datalen) {
  size_t dsti = 0;
  size_t dstlinestart = 0;
  size_t dstend = dstsize - 1;
  for (size_t srci = 0; srci < datalen; srci++) {
    if (dsti == dstend)
      return -1; // dst not large enough
    if (dsti - dstlinestart >= 80) {
      dst[dsti++] = '\n';
      dstlinestart = dsti;
      if (dsti == dstend)
        return -1;
    }
    char c = data[srci];
    if (c != '"' && !std::isspace(c) && std::isprint(c)) {
      dst[dsti++] = c;
    } else {
      if (dsti + 2 > dstend)
        return -1; // dst not large enough
      dst[dsti++] = '\\';
      switch (c) {
        case '\t': dst[dsti++] = 't'; break;
        case '\n': dst[dsti++] = 'n'; break;
        case '\r': dst[dsti++] = 'r'; break;
        case ' ':  dst[dsti++] = 's'; break;
        case '"':  dst[dsti++] = c; break;
        default: // \xHH
          if (dsti + 3 > dstend)
            return -1; // dst not large enough
          sprintf(&dst[dsti], "x%X02", c);
          dsti += 3;
          break;
      }
    }
  }
  dst[dsti] = 0;
  return (ssize_t)dsti;
}

// // little unit test for debugFmtBytes
// __attribute__((constructor)) static void test_debugFmtBytes() {
//   char buf[256];
//   if (!debugFmtBytes(buf, sizeof(buf), "hello\x0Aworld", 12)) {
//     puts("debugFmtBytes failed");
//     ::exit(1);
//   }
//   printf("buf: %s\n", buf);
//   ::exit(0);
// }
