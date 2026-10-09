#ifndef SDCARD_TEST_DFS_H
#define SDCARD_TEST_DFS_H
#include <rtdevice.h>
struct dfs_filesystem { const char *path; };
char *dfs_normalize_path(const char *directory, const char *filename);
const char *dfs_filesystem_get_mounted_path(rt_device_t device);
struct dfs_filesystem *dfs_filesystem_lookup(const char *path);
int dfs_mount(const char *device, const char *path, const char *type, unsigned flags, const void *data);
int dfs_unmount(const char *path);
void dfs_lock(void);
void dfs_unlock(void);
#endif
