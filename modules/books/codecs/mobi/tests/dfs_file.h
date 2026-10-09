#ifndef BOOK_TEST_DFS_FILE_H
#define BOOK_TEST_DFS_FILE_H
#include <stddef.h>
struct dfs_fd { int fd; long size; };
int dfs_file_open(struct dfs_fd *, const char *, int);
int dfs_file_read(struct dfs_fd *, void *, size_t);
int dfs_file_lseek(struct dfs_fd *, long);
int dfs_file_close(struct dfs_fd *);
#endif
