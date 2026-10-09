#ifndef WORDS_TEST_POSIX_H
#define WORDS_TEST_POSIX_H
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
typedef int64_t off_t;
#define stat test_stat
struct stat { off_t st_size; unsigned st_mode; };
struct statfs { uint32_t f_blocks, f_bfree, f_bsize; };
int statfs(const char *path, struct statfs *value);
struct dirent { char d_name[256]; };
typedef struct { unsigned index; char path[512]; struct dirent entry; } DIR;
#define S_ISREG(mode) ((mode) == 1)
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 4
#define O_TRUNC 8
int test_open(const char *path, int flags, ...);
int test_read(int fd, void *buffer, size_t size);
int test_write(int fd, const void *buffer, size_t size);
off_t test_lseek(int fd, off_t offset, int whence);
int test_close(int fd);
int test_fsync(int fd);
int test_stat(const char *path, struct stat *value);
int test_rename(const char *from, const char *to);
int test_unlink(const char *path);
DIR *test_opendir(const char *path);
struct dirent *test_readdir(DIR *dir);
int test_closedir(DIR *dir);
#define open test_open
#define read test_read
#define write test_write
#define lseek test_lseek
#define close test_close
#define fsync test_fsync
#define rename test_rename
#define unlink test_unlink
#define opendir test_opendir
#define readdir test_readdir
#define closedir test_closedir
#endif
