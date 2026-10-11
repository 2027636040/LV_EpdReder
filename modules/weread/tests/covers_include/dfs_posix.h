#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#define stat _stat
#define fstat _fstat
#define open _open
#define close _close
#define read _read
#define lseek _lseek
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#else
#include <unistd.h>
#endif
