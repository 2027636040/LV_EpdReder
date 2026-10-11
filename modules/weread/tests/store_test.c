#include "platform/epd_app.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include "../../weread/weread_store.c"

static bool mounted = true, fail_alloc, fail_write;
static unsigned allocations;
void *epd_app_alloc(size_t size, int region)
{
    (void)region;
    if (fail_alloc) return NULL;
    void *memory = malloc(size);
    if (memory) ++allocations;
    return memory;
}
void epd_app_free(void *memory) { if (memory) { assert(allocations); --allocations; free(memory); } }
bool storage_app_available(const char *id) { assert(!strcmp(id, "weread")); return mounted; }
bool storage_app_path(char *out, size_t size, const char *id, storage_app_area_t area, const char *relative)
{
    assert(!strcmp(id, "weread"));
    if (strstr(relative, "..") || strchr(relative, '\\')) return false;
    int n = snprintf(out, size, "store/%u/%s", (unsigned)area, relative);
    return mounted && n >= 0 && (size_t)n < size;
}
bool storage_mkdirs(const char *path)
{
    if (!mounted) return false;
    char copy[512]; snprintf(copy, sizeof(copy), "%s", path);
    for (char *p = copy; *p; ++p) if (*p == '/') { *p = 0; mkdir(copy, 0700); *p = '/'; }
    return !mkdir(copy, 0700) || errno == EEXIST;
}
mz_ulong mz_crc32(mz_ulong initial, const unsigned char *bytes, size_t size)
{
    uint32_t crc = ~(uint32_t)initial;
    for (size_t i = 0; i < size; ++i)
    {
        crc ^= bytes[i];
        for (unsigned j = 0; j < 8; ++j) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}
uint32_t storage_crc32(const void *data, size_t size) { return mz_crc32(0, data, size); }
bool storage_file_replace(const char *path, const void *data, size_t size)
{
    if (!mounted || fail_write) return false;
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    bool ok = fwrite(data, 1, size, file) == size;
    return !fclose(file) && ok;
}
bool storage_file_recover(const char *path) { (void)path; return mounted; }
void storage_lock(void) {}
void storage_unlock(void) {}

int main(void)
{
    const char text[] = "正文与标题";
    size_t size;
    char a[512], b[512], path[512];
    assert(weread_store_path(a, STORAGE_APP_DATA, "account1", "book", "position"));
    assert(weread_store_path(b, STORAGE_APP_DATA, "account2", "book", "position"));
    assert(strcmp(a, b));
    assert(weread_store_path(a, STORAGE_APP_CACHE, "a/b", "../book", "catalog"));
    assert(!strstr(a, ".."));
    assert(weread_store_save(STORAGE_APP_DATA, "a", "b", "position", text, sizeof(text)));
    char *data = weread_store_load(STORAGE_APP_DATA, "a", "b", "position", &size);
    assert(data && size == sizeof(text) && !memcmp(data, text, size)); epd_app_free(data);
    assert(!weread_store_load(STORAGE_APP_DATA, "other", "b", "position", &size));
    fail_alloc = true;
    assert(!weread_store_load(STORAGE_APP_DATA, "a", "b", "position", &size));
    assert(!weread_store_save(STORAGE_APP_DATA, "a", "b", "position", text, sizeof(text)));
    fail_alloc = false;
    assert(weread_store_path(path, STORAGE_APP_DATA, "a", "b", "position"));
    FILE *file = fopen(path, "r+b"); assert(file); fseek(file, sizeof(store_header_t), SEEK_SET); fputc('!', file); fclose(file);
    assert(!weread_store_load(STORAGE_APP_DATA, "a", "b", "position", &size));
    uint32_t signature, signature2;
    assert(weread_store_chapter("a", "b", "12", 10, false, text, sizeof(text), a, &signature));
    assert(weread_store_cached_chapter("a", "b", "12", 10, b, &signature2));
    assert(!strcmp(a, b) && signature == signature2);
    assert(!weread_store_cached_chapter("a", "b", "12", 11, b, &signature2));
    assert(!weread_store_cached_chapter("a", "b", "12", 0, b, &signature2));
    fail_write = true;
    assert(!weread_store_chapter("a", "b", "12", 11, true, "new", 3, path, &signature2));
    fail_write = false;
    assert(weread_store_cached_chapter("a", "b", "12", 10, path, &signature2));
    assert(!strcmp(a, path) && signature == signature2);
    assert(weread_store_chapter("a", "b", "12", 11, true, "new", 3, b, &signature2));
    assert(!weread_store_cached_chapter("a", "b", "12", 10, path, &signature2));
    assert(weread_store_cached_chapter("a", "b", "12", 11, path, &signature2));
    assert(strcmp(a, b) && signature != signature2);
    assert(access(a, F_OK) == 0); /* Active reader's old immutable chapter is retained. */
    file = fopen(b, "r+b"); assert(file); fputc('!', file); fclose(file);
    assert(!weread_store_cached_chapter("a", "b", "12", 11, path, &signature2));
    assert(weread_store_save(STORAGE_APP_DATA, NULL, NULL, "session", "secret", 6));
    assert(weread_store_logout());
    data = weread_store_load(STORAGE_APP_DATA, NULL, NULL, "session", &size);
    assert(data && !size && !*data); epd_app_free(data);
    mounted = false;
    assert(!weread_store_load(STORAGE_APP_DATA, NULL, NULL, "session", &size));
    assert(!weread_store_cached_chapter("a", "b", "12", 11, path, &signature2));
    assert(!weread_store_save(STORAGE_APP_DATA, NULL, NULL, "session", text, sizeof(text)));
    mounted = true;
    char huge[512]; memset(huge, 'a', sizeof(huge) - 1); huge[511] = 0;
    assert(!weread_store_path(path, STORAGE_APP_DATA, huge, "b", "position"));
    assert(!allocations);
    puts("weread store: account isolation, roundtrip, CRC, allocation/write failure, revision invalidation, immutable chapters, logout, removal, paths PASS");
    return 0;
}
