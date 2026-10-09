#include <rtthread.h>
#define ROMFS_DIRENT_DIR 1
struct romfs_dirent { int type; const char *name; const rt_uint8_t *data; size_t size; };
