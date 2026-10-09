#include <stdint.h>
struct statfs { uint32_t f_blocks, f_bfree, f_bsize; };
int dfs_mount(const char *, const char *, const char *, unsigned long, const void *);
int dfs_statfs(const char *, struct statfs *);
