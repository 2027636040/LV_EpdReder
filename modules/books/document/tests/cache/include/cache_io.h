#ifndef CACHE_TEST_IO_H
#define CACHE_TEST_IO_H
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <time.h>
int cache_test_open(const char *path, int flags, ...);
int cache_test_stat(const char *path, struct stat *st);
int cache_test_mkdir(const char *path, mode_t mode);
int cache_test_unlink(const char *path);
int cache_test_rename(const char *from, const char *to);
DIR *cache_test_opendir(const char *path);
ssize_t cache_test_read(int fd, void *data, size_t size);
ssize_t cache_test_write(int fd, const void *data, size_t size);
int cache_test_fsync(int fd);
time_t cache_test_time(time_t *out);
#ifndef CACHE_TEST_IO_IMPLEMENTATION
#define open cache_test_open
#define stat(path, st) cache_test_stat(path, st)
#define mkdir cache_test_mkdir
#define unlink cache_test_unlink
#define rename cache_test_rename
#define opendir cache_test_opendir
#define read cache_test_read
#define write cache_test_write
#define fsync cache_test_fsync
#define time cache_test_time
#endif
#endif
