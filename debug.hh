#pragma once
#include <unistd.h>

// debugFmtBytes writes a human-readable representation of data to dst.
// Returns the number of bytes added to dst, or -1 if dst was not large enough.
ssize_t debugFmtBytes(char* dst, size_t dstsize, const char* data, size_t datalen);
