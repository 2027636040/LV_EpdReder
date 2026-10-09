#include <dfs_fs.h>
struct dfs_fd { unsigned ref_count; struct dfs_filesystem *fs; };
struct dfs_fdtable { unsigned maxfd; struct dfs_fd **fds; };
struct dfs_fdtable *dfs_fdtable_get(void);
