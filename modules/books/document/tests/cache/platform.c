#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "../../document_cache.h"
#include "platform/epd_app.h"
#include "platform.h"

unsigned test_allocations, test_io, test_source_opens;
int test_fail_alloc = -1, test_fail_write_after = -1, test_fail_rename_at = -1;
bool test_fail_sync, test_short_write, test_short_read, test_hotplug;
uint32_t test_generation = 1, test_now = 100;
storage_volume_t test_app_volume = STORAGE_FLASH;
char test_source[BOOKSHELF_PATH_MAX];
static char fixture[1024];
static unsigned lock_depth;

static const char *mapped(const char *path, char out[2048])
{
    assert(lock_depth);
    ++test_io;
    assert(!strncmp(path, "/flash", 6) || !strncmp(path, "/sdcard", 7));
    int n = snprintf(out, 2048, "%s%s", fixture, path);
    assert(n > 0 && n < 2048);
    return out;
}
int cache_test_open(const char *path, int flags, ...)
{
    char p[2048];
    if (*test_source && !strcmp(test_source, path)) ++test_source_opens;
    return open(mapped(path, p), flags, 0600);
}
int cache_test_stat(const char *path, struct stat *st)
{
    char p[2048];
    int result = stat(mapped(path, p), st);
    if (result) errno = -errno; /* Match RT-Thread DFS errno convention. */
    return result;
}
int cache_test_mkdir(const char *path, mode_t mode)
{ char p[2048]; (void)mode; return mkdir(mapped(path, p), 0700); }
int cache_test_unlink(const char *path)
{ char p[2048]; return unlink(mapped(path, p)); }
int cache_test_rename(const char *from, const char *to)
{
    char a[2048], b[2048];
    if (test_fail_rename_at == 0) { errno = EIO; return -1; }
    if (test_fail_rename_at > 0) --test_fail_rename_at;
    mapped(from, a); mapped(to, b);
    return rename(a, b);
}
DIR *cache_test_opendir(const char *path)
{ char p[2048]; return opendir(mapped(path, p)); }
ssize_t cache_test_read(int fd, void *data, size_t size)
{
    assert(lock_depth); ++test_io;
    if (test_short_read && size > 3) size = 3;
    return read(fd, data, size);
}
ssize_t cache_test_write(int fd, const void *data, size_t size)
{
    assert(lock_depth); ++test_io;
    if (!test_fail_write_after) { errno = ENOSPC; return -1; }
    if (test_fail_write_after > 0 && size > (size_t)test_fail_write_after) size = (size_t)test_fail_write_after;
    if (test_short_write && size > 3) size = 3;
    ssize_t n = write(fd, data, size);
    if (n > 0 && test_fail_write_after > 0) test_fail_write_after -= (int)n;
    return n;
}
int cache_test_fsync(int fd)
{
    assert(lock_depth); ++test_io;
    if (test_fail_sync) { errno = ENOSPC; return -1; }
    return fsync(fd);
}
time_t cache_test_time(time_t *out)
{ if (out) *out = test_now; return test_now; }
void storage_lock(void)
{
    if (!lock_depth && test_hotplug) { ++test_generation; test_hotplug = false; }
    ++lock_depth;
}
void storage_unlock(void) { assert(lock_depth); --lock_depth; }
const char *storage_path_root(const char *p)
{ return !strncmp(p, "/sdcard", 7) ? STORAGE_SD_ROOT : STORAGE_FLASH_ROOT; }
bool storage_available(storage_volume_t volume) { return volume < STORAGE_COUNT; }
bool storage_path_available(const char *path) { return path && *path; }
bool storage_session_valid(storage_volume_t volume, uint32_t generation)
{ return volume == STORAGE_FLASH || generation == test_generation; }
bool storage_changing(void) { return false; }
uint32_t storage_card_session(void) { return test_generation; }
uint32_t storage_revision(void) { return test_generation; }
bool app_module_storage(const char *id, storage_volume_t *volume, uint32_t *generation)
{ assert(!strcmp(id, "books")); *volume = test_app_volume; *generation = test_generation; return true; }
void *epd_app_alloc(size_t size, int region)
{
    (void)region;
    if (test_fail_alloc == 0) return NULL;
    if (test_fail_alloc > 0) --test_fail_alloc;
    void *p = calloc(1, size);
    if (p) ++test_allocations;
    return p;
}
void *epd_app_realloc(void *p, size_t size, int region)
{
    if (!p) return epd_app_alloc(size, region);
    if (test_fail_alloc == 0) return NULL;
    if (test_fail_alloc > 0) --test_fail_alloc;
    return realloc(p, size);
}
void epd_app_free(void *p)
{ if (p) { assert(test_allocations); --test_allocations; free(p); } }
#ifndef CACHE_REAL_FORMATS
void *bf_alloc(size_t size) { return epd_app_alloc(size, 1); }
void bf_free(void *p) { epd_app_free(p); }
char *bf_strdup(const char *text)
{ size_t n = strlen(text) + 1; char *p = bf_alloc(n); if (p) memcpy(p, text, n); return p; }
#endif

void test_platform_init(const char *directory)
{
    assert(strlen(directory) < sizeof(fixture)); strcpy(fixture, directory);
    storage_lock();
    assert(!cache_test_mkdir("/flash", 0700));
    assert(!cache_test_mkdir("/sdcard", 0700));
    storage_unlock();
    assert(storage_mkdirs("/flash/cache/books/documents"));
    assert(storage_mkdirs("/sdcard/.epd/cache/books/documents"));
    assert(storage_mkdirs("/flash/assets"));
    assert(storage_mkdirs("/sdcard/assets"));
}
void test_put(const char *path, const void *data, size_t size)
{
    storage_lock();
    int fd = cache_test_open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600); assert(fd >= 0);
    assert(write(fd, data, size) == (ssize_t)size); assert(!close(fd));
    storage_unlock();
}
void test_sparse(const char *path, uint32_t size)
{
    storage_lock();
    int fd = cache_test_open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600); assert(fd >= 0);
    assert(!ftruncate(fd, size)); assert(!close(fd)); storage_unlock();
}
bool test_exists(const char *path)
{
    struct stat st; storage_lock(); bool found = !cache_test_stat(path, &st); storage_unlock(); return found;
}
void test_remove(const char *path)
{ storage_lock(); assert(!cache_test_unlink(path)); storage_unlock(); }
