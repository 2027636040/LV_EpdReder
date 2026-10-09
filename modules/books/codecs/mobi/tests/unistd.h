#ifdef _WIN32
#include <io.h>
#else
#include_next <unistd.h>
#endif
int test_open(const char *, int);
int test_read(int, void *, unsigned);
long test_lseek(int, long, int);
int test_close(int);
#define open test_open
#define read test_read
#define lseek test_lseek
#define close test_close
