#ifndef BOOK_TEST_COMPAT_H
#define BOOK_TEST_COMPAT_H
#ifdef _MSC_VER
#define __attribute__(x)
#include <io.h>
#endif
#include <stdint.h>
#include <fcntl.h>
#ifndef O_BINARY
#ifdef _WIN32
#define O_BINARY _O_BINARY
#else
#define O_BINARY 0
#endif
#endif
#endif
