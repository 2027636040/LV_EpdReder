#include "weread_store.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <miniz.h>

#define STORE_MAGIC 0x57525331u
#define STORE_VERSION 1u
typedef struct { uint32_t magic, version, size, crc; } store_header_t;
typedef struct
{
    uint32_t version, size, crc, html;
    int64_t content_revision;
} chapter_meta_t;

static bool append_key(char *out, size_t size, const char *key)
{
    static const char hex[] = "0123456789abcdef";
    size_t used = strlen(out), bytes = strlen(key);
    if (bytes > (size - used - 1) / 2) return false;
    for (size_t i = 0; i < bytes; ++i)
    {
        unsigned c = (unsigned char)key[i];
        out[used++] = hex[c >> 4]; out[used++] = hex[c & 15];
    }
    out[used] = 0;
    return true;
}

bool weread_store_path(char *path, storage_app_area_t area, const char *account,
                       const char *book, const char *name)
{
    char relative[STORAGE_PATH_MAX] = "";
    if (!name || !*name || !storage_app_available("weread")) return false;
    if (account)
    {
        if (!*account || !append_key(relative, sizeof(relative), account)) return false;
        if (strlen(relative) + 1 >= sizeof(relative)) return false;
        strcat(relative, "/");
    }
    if (book)
    {
        if (!*book || !append_key(relative, sizeof(relative), book)) return false;
        if (strlen(relative) + 1 >= sizeof(relative)) return false;
        strcat(relative, "/");
    }
    if (strlen(relative) + strlen(name) >= sizeof(relative)) return false;
    strcat(relative, name);
    return storage_app_path(path, STORAGE_PATH_MAX, "weread", area, relative);
}

static bool parent_create(const char *path)
{
    char parent[STORAGE_PATH_MAX];
    snprintf(parent, sizeof(parent), "%s", path);
    char *slash = strrchr(parent, '/');
    if (!slash) return false;
    *slash = 0;
    return storage_mkdirs(parent);
}

static bool read_exact(int fd, void *data, size_t size)
{
    unsigned char *out = data;
    while (size)
    {
        if (!storage_app_available("weread")) return false;
        size_t part = size > 16384 ? 16384 : size;
        int n = read(fd, out, part);
        if (n <= 0) return false;
        out += n; size -= (size_t)n;
    }
    return true;
}

bool weread_store_save(storage_app_area_t area, const char *account, const char *book,
                       const char *name, const void *data, size_t size)
{
    char path[STORAGE_PATH_MAX];
    if ((!data && size) || size > UINT32_MAX || size > SIZE_MAX - sizeof(store_header_t) ||
        !weread_store_path(path, area, account, book, name) || !parent_create(path)) return false;
    store_header_t *record = epd_app_alloc(sizeof(*record) + size, EPD_APP_PSRAM);
    if (!record) return false;
    record->magic = STORE_MAGIC; record->version = STORE_VERSION;
    record->size = (uint32_t)size; record->crc = storage_crc32(data, size);
    if (size) memcpy(record + 1, data, size);
    bool ok = storage_app_available("weread") &&
              storage_file_replace(path, record, sizeof(*record) + size);
    epd_app_free(record);
    return ok;
}

void *weread_store_load(storage_app_area_t area, const char *account, const char *book,
                        const char *name, size_t *size)
{
    char path[STORAGE_PATH_MAX];
    void *data = NULL;
    *size = 0;
    if (!weread_store_path(path, area, account, book, name)) return NULL;
    storage_lock();
    int fd = storage_file_recover(path) ? open(path, O_RDONLY, 0) : -1;
    store_header_t header;
    struct stat st;
    bool ok = fd >= 0 && !fstat(fd, &st) && read_exact(fd, &header, sizeof(header)) &&
        header.magic == STORE_MAGIC && header.version == STORE_VERSION &&
        (uint64_t)st.st_size == (uint64_t)sizeof(header) + header.size;
    size_t payload_size = ok ? header.size : 0;
    if (payload_size == SIZE_MAX) ok = false;
    if (ok)
    {
        data = epd_app_alloc((size_t)header.size + 1, EPD_APP_PSRAM);
        ok = data && read_exact(fd, data, header.size) &&
             storage_crc32(data, header.size) == header.crc;
    }
    if (fd >= 0) close(fd);
    storage_unlock();
    if (!ok) { epd_app_free(data); return NULL; }
    ((char *)data)[header.size] = 0;
    *size = header.size;
    return data;
}

static bool chapter_name(char *name, size_t capacity, const char *chapter, const char *extension)
{
    name[0] = 0;
    if (!chapter || !*chapter || !append_key(name, capacity, chapter) ||
        strlen(name) + strlen(extension) >= capacity) return false;
    strcat(name, extension);
    return true;
}

bool weread_store_chapter(const char *account, const char *book, const char *chapter,
                          int64_t content_revision, bool html, const void *data, size_t size,
                          char path[STORAGE_PATH_MAX], uint32_t *signature)
{
    char name[STORAGE_PATH_MAX], suffix[32];
    if (!data || !size || size > UINT32_MAX) return false;
    chapter_meta_t meta = {2, (uint32_t)size, storage_crc32(data, size), html, content_revision};
    snprintf(suffix, sizeof(suffix), "-%08lx.%s", (unsigned long)meta.crc, html ? "xhtml" : "txt");
    if (!chapter_name(name, sizeof(name), chapter, suffix) ||
        !weread_store_path(path, STORAGE_APP_CACHE, account, book, name) || !parent_create(path) ||
        !storage_file_replace(path, data, size) ||
        !chapter_name(name, sizeof(name), chapter, ".meta") ||
        !weread_store_save(STORAGE_APP_CACHE, account, book, name, &meta, sizeof(meta))) return false;
    *signature = meta.crc;
    return true;
}

bool weread_store_cached_chapter(const char *account, const char *book, const char *chapter,
                                 int64_t content_revision,
                                 char path[STORAGE_PATH_MAX], uint32_t *signature)
{
    char name[STORAGE_PATH_MAX], suffix[32];
    size_t size;
    if (!chapter_name(name, sizeof(name), chapter, ".meta")) return false;
    chapter_meta_t *meta = weread_store_load(STORAGE_APP_CACHE, account, book, name, &size);
    if (!meta) return false;
    bool ok = size == sizeof(*meta) && meta->version == 2 && meta->size && meta->html <= 1 &&
              meta->content_revision == content_revision;
    if (ok)
    {
        snprintf(suffix, sizeof(suffix), "-%08lx.%s", (unsigned long)meta->crc, meta->html ? "xhtml" : "txt");
        ok = chapter_name(name, sizeof(name), chapter, suffix) &&
             weread_store_path(path, STORAGE_APP_CACHE, account, book, name);
    }
    if (ok)
    {
        struct stat st;
        unsigned char buffer[2048];
        uint32_t crc = 0;
        storage_lock();
        int fd = storage_app_available("weread") ? open(path, O_RDONLY, 0) : -1;
        ok = fd >= 0 && !fstat(fd, &st) && (uint64_t)st.st_size == meta->size;
        size_t remaining = ok ? meta->size : 0;
        while (ok && remaining)
        {
            size_t take = remaining > sizeof(buffer) ? sizeof(buffer) : remaining;
            ok = read_exact(fd, buffer, take);
            if (ok) { crc = mz_crc32(crc, buffer, take); remaining -= take; }
        }
        if (fd >= 0 && close(fd)) ok = false;
        ok = ok && crc == meta->crc;
        storage_unlock();
    }
    /* Chapter files are immutable and published only after the atomic write. */
    if (ok) *signature = meta->crc;
    epd_app_free(meta);
    return ok;
}

bool weread_store_logout(void)
{
    /* A valid empty record supersedes any recoverable old session file. */
    return weread_store_save(STORAGE_APP_DATA, NULL, NULL, "session", NULL, 0);
}
