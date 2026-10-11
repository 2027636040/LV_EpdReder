/* SPDX-License-Identifier: Apache-2.0 */
#include "document.h"
#include "document_cache.h"
#include "../formats/formats.h"
#include <rthw.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

#define DOCUMENT_IO_WINDOW 8192u
struct document_reader {
    struct document_reader *next;
    document_t *document;
    int descriptor;
    uint32_t visible, position, window_start, window_used;
    uint8_t *window;
};
struct document_image_request {
    struct document_image_request *next;
    document_t *document;
    uint32_t offset;
    unsigned width, height;
    bool active, done, released;
    int result;
    document_bitmap_t bitmap;
};
typedef struct document_save {
    struct document_save *next;
    uint32_t generation;
    size_t length;
    char path[STORAGE_PATH_MAX];
    uint8_t data[];
} document_save_t;
typedef struct {
    uint32_t magic, version, content, layout, end, bytes, checksum;
} document_index_header_t;
#define DOCUMENT_INDEX_MAGIC 0x44584942u

struct document {
    struct document *next;
    bookshelf_item_t file;
    document_source_t source;
    char app_id[64], directory[STORAGE_PATH_MAX], identity[BOOKSHELF_PATH_MAX];
    document_format_t format;
    uint32_t generation, serial, end, link;
    uint32_t cache_generation;
    uint32_t pending_end, input_publication;
    rt_tick_t last_publish, started;
    int output, input, cache_result;
    uint8_t *write_buffer;
    unsigned write_used;
    document_reader_t *readers, *default_reader;
    bool persistent, space_retried, cache_bad, cache_touch, temp_owned;
    document_cache_t *cached;
    uint32_t read_calls, write_calls, seek_calls, open_calls, sync_calls;
    uint32_t read_bytes, write_bytes;
    char cache[STORAGE_PATH_MAX];
    char work_error[96];
    char cache_error[96];
    void *library;
    const book_convert_api_t *converter;
    bool cancelled, claimed, stopped, released;
    document_status_t status;
};
static document_t *documents;
static document_t *worker_document;
static epd_service_t *worker_service;
static bool worker_stopping;
static uint32_t sequence;
static bool cache_initialized[STORAGE_COUNT];
static document_image_request_t *image_requests;
static document_save_t *saves;
static void image_work(document_t *only);
static void save_work(void);
static bool image_pending(document_t *d);
static bool image_referenced(document_t *d);
static void image_collect(void);
static void readers_close(document_t *d);

static void *allocate(size_t bytes) { return epd_app_alloc(bytes, EPD_APP_PSRAM); }
static void *resize(void *p, size_t bytes) { return epd_app_realloc(p, bytes, EPD_APP_PSRAM); }
static const book_allocator_t allocator = {allocate, resize, epd_app_free};

static bool extension(const char *path, const char *suffix)
{
    const char *p = strrchr(path, '.');
    if (!p) return false;
    while (*p && *suffix && tolower((unsigned char)*p) == *suffix) { ++p; ++suffix; }
    return !*p && !*suffix;
}

document_format_t document_format(const char *path, bool directory)
{
    return book_format_detect(path, directory);
}

const char *document_format_name(document_format_t format)
{
    return book_format_name(format);
}

static uint32_t hash_bytes(uint32_t h, const void *data, size_t n)
{
    const unsigned char *p = data;
    while (n--) h = (h ^ *p++) * 16777619u;
    return h;
}

bool document_split_identity(const char *path, uint32_t *size, uint32_t *signature)
{
    char child[BOOKSHELF_PATH_MAX];
    if (snprintf(child, sizeof(child), "%s/basepackage.zip", path) >= (int)sizeof(child)) return false;
    struct stat st;
    if (stat(child, &st) || !S_ISREG(st.st_mode)) return false;
    DIR *dir = opendir(path);
    if (!dir) return false;
    uint32_t tag = 0, total = 0, chapters = 0;
    struct dirent *entry;
    bool valid = true;
    while ((entry = readdir(dir)) != NULL)
    {
        if (entry->d_name[0] == '.' ||
            (!extension(entry->d_name, ".zip") && strcmp(entry->d_name, "meta.json"))) continue;
        if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >= (int)sizeof(child) ||
            stat(child, &st) || !S_ISREG(st.st_mode)) { valid = false; break; }
        uint32_t content = (uint32_t)st.st_mtime;
        if (!strcmp(storage_path_root(path), STORAGE_FLASH_ROOT) &&
            !storage_file_signature(child, &content)) { valid = false; break; }
        uint32_t values[] = {(uint32_t)st.st_size, content};
        tag += hash_bytes(hash_bytes(2166136261u, entry->d_name, strlen(entry->d_name)), values, sizeof(values));
        total += (uint32_t)st.st_size;
        if (extension(entry->d_name, ".zip") && strcmp(entry->d_name, "basepackage.zip")) ++chapters;
    }
    closedir(dir);
    if (size) *size = total;
    if (signature) *signature = tag;
    return valid && chapters;
}

static bool media_valid(const char *path, uint32_t generation)
{
    if (!path[0]) return true;
    return storage_path_available(path) &&
        (strcmp(storage_path_root(path), STORAGE_SD_ROOT) ||
         (generation == storage_card_session() && !storage_changing()));
}
static bool cancelled(void *user)
{
    document_t *d = user;
    return d->cancelled || (worker_service && epd_service_cancelled(worker_service)) ||
           (d->source.cancelled && d->source.cancelled(d->source.user)) ||
           !media_valid(d->file.path, d->generation) ||
           (d->source.convert && !media_valid(d->directory, d->cache_generation)) ||
           !media_valid(d->cache, d->cache_generation);
}

static void wake_worker(void)
{
    if (worker_service) epd_service_wake(worker_service);
}

static int cache_failure(document_t *d, const char *operation, int error)
{
    if (error < 0) error = -error;
    if (!d->cache_error[0])
    {
        snprintf(d->cache_error, sizeof(d->cache_error), "%s", error == ENOSPC ?
                 "文档缓存存储空间不足，请释放空间后重开" : "文档缓存写入失败，请检查存储后重开");
        rt_kprintf("[document] cache %s failed: path=%s offset=%u errno=%d\n",
                   operation, d->cache, d->end, error);
    }
    d->cache_result = error == ENOSPC ? BOOK_NO_SPACE : BOOK_IO_ERROR;
    return d->cache_result;
}

static int cache_room(document_t *d, size_t length)
{
    /* DFS uses signed 32-bit file offsets on this target. */
    if (d->end <= INT32_MAX && length <= (uint32_t)INT32_MAX - d->end) return BOOK_OK;
    snprintf(d->cache_error, sizeof(d->cache_error), "文档缓存超出文件寻址范围");
    return d->cache_result = BOOK_UNSUPPORTED;
}

static int write_direct(document_t *d, const void *data, size_t length)
{
    if (cancelled(d)) return BOOK_CANCELLED;
    const uint8_t *p = data;
    while (length)
    {
        size_t chunk = length > 4096 ? 4096 : length;
        storage_lock();
        if (cancelled(d)) { storage_unlock(); return BOOK_CANCELLED; }
        errno = 0;
        int n = write(d->output, p, chunk);
        ++d->write_calls;
        int error = errno;
        storage_unlock();
        if (n <= 0 && (error == ENOSPC || error == -ENOSPC) && !d->space_retried && d->cached)
        {
            d->space_retried = true;
            if (document_cache_reclaim(d->cached)) continue;
        }
        if (n <= 0) return cache_failure(d, "write", error);
        d->write_bytes += n; p += n; length -= n;
        if (cancelled(d)) return BOOK_CANCELLED;
    }
    return BOOK_OK;
}

static int write_flush(document_t *d)
{
    if (!d->write_used) return BOOK_OK;
    int result = write_direct(d, d->write_buffer, d->write_used);
    /* A failed/partial flush makes this conversion unusable; never replay it. */
    d->write_used = 0;
    return result;
}

static int write_all(document_t *d, const void *data, size_t length)
{
    if (cancelled(d)) return BOOK_CANCELLED;
    int result = cache_room(d, length);
    if (result != BOOK_OK) return result;
    const uint8_t *p = data;
    while (length)
    {
        size_t n = length;
        if (!d->write_buffer || (!d->write_used && length >= DOCUMENT_IO_WINDOW))
        {
            result = write_direct(d, p, length);
            if (result != BOOK_OK) return result;
        }
        else
        {
            if (n > DOCUMENT_IO_WINDOW - d->write_used) n = DOCUMENT_IO_WINDOW - d->write_used;
            memcpy(d->write_buffer + d->write_used, p, n);
            d->write_used += n;
            if (d->write_used == DOCUMENT_IO_WINDOW && (result = write_flush(d)) != BOOK_OK) return result;
        }
        d->end += n; p += n; length -= n;
    }
    return BOOK_OK;
}

static int cache_publish(document_t *d, bool force)
{
    uint32_t bytes = d->pending_end - d->status.end;
    if (!bytes) return BOOK_OK;
    rt_tick_t now = rt_tick_get();
    uint32_t batch = d->status.end ? 8192 : 2048;
    if (!force && bytes < batch && now - d->last_publish < rt_tick_from_millisecond(50)) return BOOK_OK;
    int flushed = write_flush(d);
    if (flushed != BOOK_OK) return flushed;
    storage_lock();
    int result = BOOK_CANCELLED;
    if (!cancelled(d))
    {
        errno = 0;
        int synced = fsync(d->output);
        if (synced && (errno == ENOSPC || errno == -ENOSPC) && !d->space_retried && d->cached)
        {
            d->space_retried = true;
            if (document_cache_reclaim(d->cached)) synced = fsync(d->output);
        }
        result = synced == 0 ? BOOK_OK : cache_failure(d, "sync", errno);
        ++d->sync_calls;
    }
    storage_unlock();
    if (result != BOOK_OK) return result;
    bool first = d->status.end == 0;
    rt_base_t key = rt_hw_interrupt_disable();
    d->status.end = d->pending_end;
    ++d->status.publication;
    rt_hw_interrupt_enable(key);
    d->last_publish = now;
    if (first)
        rt_kprintf("[document] first-publish: %u bytes, %lu ms\n", d->pending_end,
            (unsigned long)((uint64_t)(rt_tick_get() - d->started) * 1000 / RT_TICK_PER_SECOND));
    /* Only complete, synced records are visible to cooperative pixel jobs. */
    if (worker_document == d) image_work(d);
    return BOOK_OK;
}

static int record_complete(document_t *d)
{
    d->pending_end = d->end;
    return cache_publish(d, false);
}

static int record_write(document_t *d, unsigned kind, unsigned style, const void *data, size_t length)
{
    if (length > INT32_MAX - sizeof(document_record_t)) return BOOK_UNSUPPORTED;
    int room = cache_room(d, sizeof(document_record_t) + length);
    if (room != BOOK_OK) return room;
    document_record_t header = {kind, (uint32_t)length, style, d->link};
    int result = write_all(d, &header, sizeof(header));
    if (result == BOOK_OK && length) result = write_all(d, data, length);
    if (result == BOOK_OK) result = record_complete(d);
    return result;
}

static int sink_text(void *user, const char *text, size_t length, unsigned style)
{
    document_t *d = user;
    while (length)
    {
        size_t n = length > DOCUMENT_TEXT_LIMIT ? DOCUMENT_TEXT_LIMIT : length;
        if (n < length) while (n && ((unsigned char)text[n] & 0xc0) == 0x80) --n;
        if (!n) return BOOK_ERROR;
        int result = record_write(d, DOCUMENT_TEXT, style, text, n);
        if (result != BOOK_OK) return result;
        text += n; length -= n;
    }
    return BOOK_OK;
}
static int sink_paragraph(void *user) { return record_write(user, DOCUMENT_PARAGRAPH, 0, NULL, 0); }
static int sink_anchor(void *user, const char *id, const char *title, unsigned level)
{
    char data[1024];
    size_t a = id ? strlen(id) : 0, b = title ? strlen(title) : 0;
    if (a + b + 2 > sizeof(data)) return BOOK_UNSUPPORTED;
    memcpy(data, id ? id : "", a + 1);
    memcpy(data + a + 1, title ? title : "", b + 1);
    return record_write(user, DOCUMENT_ANCHOR, level, data, a + b + 2);
}
static int sink_link(void *user, const char *target, bool begin)
{
    document_t *d = user;
    if (!begin) { d->link = 0; return BOOK_OK; }
    if (!target || strlen(target) >= 1024) return BOOK_UNSUPPORTED;
    uint32_t offset = d->end;
    int result = record_write(d, DOCUMENT_LINK, 0, target, strlen(target) + 1);
    if (result == BOOK_OK) d->link = offset + 1;
    return result;
}
static int sink_metadata(void *user, const char *title, const char *author)
{
    (void)author;
    document_t *d = user;
    if (title && *title)
    {
        rt_base_t key = rt_hw_interrupt_disable();
        snprintf(d->status.title, sizeof(d->status.title), "%s", title);
        rt_hw_interrupt_enable(key);
    }
    return cancelled(d) ? BOOK_CANCELLED : BOOK_OK;
}
static int sink_image(void *user, const char *id, book_stream_t *stream)
{
    (void)id;
    document_t *d = user;
    uint8_t *gray = NULL;
    unsigned width = 0, height = 0;
    int result = document_image_decode(stream, cancelled, d, &gray, &width, &height);
    if (result != BOOK_OK) return result;
    document_image_t dimensions = {width, height};
    size_t pixels = width * height, bytes = (pixels + 1) / 2;
    for (size_t i = 0; i < pixels; i += 2)
        gray[i / 2] = (uint8_t)((((gray[i] + 8) / 17) << 4) |
                       (i + 1 < pixels ? (gray[i + 1] + 8) / 17 : 15));
    document_record_t record = {DOCUMENT_IMAGE, sizeof(dimensions) + bytes, 0, d->link};
    result = cache_room(d, sizeof(record) + record.length);
    if (result == BOOK_OK) result = write_all(d, &record, sizeof(record));
    if (result == BOOK_OK) result = write_all(d, &dimensions, sizeof(dimensions));
    if (result == BOOK_OK) result = write_all(d, gray, bytes);
    epd_app_free(gray);
    if (result == BOOK_OK) result = record_complete(d);
    return result;
}

static void result_locked(document_t *d, int result)
{
    d->status.result = result;
    if (result == BOOK_OK) return;
    const char *message = result == BOOK_CANCELLED ? "书籍读取已中断，请重新打开" :
        result == BOOK_NO_MEMORY ? "文档处理内存申请失败" :
        result == BOOK_NO_SPACE ? "文档缓存存储空间不足" :
        result == BOOK_UNSUPPORTED ? "文档包含不支持的内容或编码" :
        result == BOOK_IO_ERROR ? "无法读取书籍或写入文档缓存" : "文档损坏或解析失败";
    snprintf(d->status.error, sizeof(d->status.error), "%s", d->cache_error[0] ? d->cache_error :
             d->work_error[0] ? d->work_error : message);
}
static bool bitmap_allocate(document_bitmap_t *result, unsigned width, unsigned height)
{
    if (!width || !height || width > (UINT16_MAX - 3u) / 2u || height > UINT16_MAX) return false;
    unsigned stride = RT_ALIGN(width * 2, 4);
    if (height > (SIZE_MAX - 31u) / stride) return false;
    result->memory = allocate(stride * height + 31);
    if (!result->memory) return false;
    result->image.header.magic = LV_IMAGE_HEADER_MAGIC;
    result->image.header.cf = LV_COLOR_FORMAT_RGB565;
    result->image.header.w = width;
    result->image.header.h = height;
    result->image.header.stride = stride;
    result->image.data = (uint8_t *)RT_ALIGN((uintptr_t)result->memory, 32);
    result->image.data_size = stride * height;
    memset((void *)result->image.data, 255, result->image.data_size);
    return true;
}
static uint16_t rgb565(unsigned gray)
{
    gray = ((gray + 8) / 17) * 17;
    return ((gray & 0xf8) << 8) | ((gray & 0xfc) << 3) | (gray >> 3);
}
static void bitmap_clean(document_bitmap_t *bitmap)
{
    lv_draw_buf_t buffer = {.data = (uint8_t *)bitmap->image.data, .data_size = bitmap->image.data_size};
    epd_app_clean_draw_buffer(&buffer);
}
static bool cache_open(document_t *d)
{
    storage_lock();
    if (cancelled(d)) { storage_unlock(); return false; }
    char directory[STORAGE_PATH_MAX];
    if (d->source.convert) snprintf(directory, sizeof(directory), "%s", d->directory);
    else if (!storage_app_path(directory, sizeof(directory), "books", STORAGE_APP_CACHE, "documents"))
    { storage_unlock(); return false; }
    storage_volume_t volume = !strcmp(storage_path_root(directory), STORAGE_SD_ROOT) ? STORAGE_SD : STORAGE_FLASH;
    if (!d->source.convert) d->cache_generation = storage_card_session();
    int named = d->source.convert ?
        snprintf(d->cache, sizeof(d->cache), "%s/tmp-%08lx-%08lx.bin", directory,
                 (unsigned long)hash_bytes(2166136261u, d->app_id, strlen(d->app_id)), (unsigned long)d->serial) :
        snprintf(d->cache, sizeof(d->cache), "%s/doc-%08lx.bin", directory, (unsigned long)d->serial);
    if (named < 0 || (size_t)named >= sizeof(d->cache)) { storage_unlock(); return false; }
    if (cancelled(d)) { storage_unlock(); return false; }
    errno = 0;
    bool ok = storage_mkdirs(directory);
    if (!ok) cache_failure(d, "mkdir", errno);
    if (ok && !d->source.convert && !cache_initialized[volume])
    {
        DIR *dir = opendir(directory);
        struct dirent *entry;
        while (dir && (entry = readdir(dir)) != NULL)
        {
            if (cancelled(d)) { ok = false; break; }
            size_t n = strlen(entry->d_name);
            if (n == 16 && !strncmp(entry->d_name, "doc-", 4) && !strcmp(entry->d_name + 12, ".bin"))
            {
                char path[STORAGE_PATH_MAX];
                size_t prefix = strlen(directory);
                if (prefix + 18 > sizeof(path)) { ok = false; break; }
                memcpy(path, directory, prefix); path[prefix] = '/';
                memcpy(path + prefix + 1, entry->d_name, 17);
                unlink(path);
            }
        }
        if (dir) closedir(dir);
        cache_initialized[volume] = true;
    }
    if (ok && !cancelled(d))
    {
        do
        {
            errno = 0;
            d->output = open(d->cache, O_CREAT | O_WRONLY | (d->source.convert ? O_EXCL : O_TRUNC), 0);
            if (d->output >= 0) { d->temp_owned = true; break; }
            if (!d->source.convert || (errno != EEXIST && errno != -EEXIST) || cancelled(d)) break;
            d->serial = ++sequence;
            snprintf(d->cache, sizeof(d->cache), "%.*s/tmp-%08lx-%08lx.bin",
                     (int)sizeof(d->cache) - 32, directory,
                     (unsigned long)hash_bytes(2166136261u, d->app_id, strlen(d->app_id)), (unsigned long)d->serial);
        } while (!cancelled(d));
        if (d->output < 0) cache_failure(d, "open", errno);
    }
    if (d->output >= 0) d->write_buffer = allocate(DOCUMENT_IO_WINDOW);
    d->last_publish = rt_tick_get();
    storage_unlock();
    return d->output >= 0;
}

static void convert(document_t *d)
{
    int result = BOOK_IO_ERROR;
    bool cacheable = true;
    rt_kprintf("[document] begin: %s (%u bytes)\n", d->file.path, d->file.size);
    if (cache_open(d))
    {
        rt_kprintf("[document] cache-open: %s\n", d->cache);
        book_sink_t sink = {d, cancelled, sink_text, sink_paragraph, sink_anchor,
                           sink_image, sink_metadata, sink_link, book_markup_convert};
        bool observing = d->cached && document_cache_observe_begin(d->cached);
        if (d->source.convert)
            result = d->source.convert(&d->source, &sink, &allocator, d->work_error, sizeof(d->work_error), &cacheable);
        else if (d->converter)
        {
            result = book_markup_begin(&allocator);
            if (result == BOOK_OK)
            {
                result = d->converter->convert(d->file.path, &sink, &allocator,
                                               d->work_error, sizeof(d->work_error));
                book_markup_end();
            }
        }
        else result = book_format_convert(d->file.path, &sink, &allocator,
                                           d->work_error, sizeof(d->work_error));
        if (observing) document_cache_observe_end(d->cached);
        if (result == BOOK_OK) result = cache_publish(d, true);
        storage_lock();
        errno = 0;
        int closed = close(d->output);
        if (closed != 0 && result == BOOK_OK) result = cache_failure(d, "close", errno);
        d->output = -1;
        storage_unlock();
        epd_app_free(d->write_buffer); d->write_buffer = NULL; d->write_used = 0;
        if (result == BOOK_OK && cacheable && observing && !cancelled(d))
        {
            storage_lock();
            if (!cancelled(d) && document_cache_prepare(d->cached))
            {
                readers_close(d);
                const char *binary = document_cache_binary(d->cached);
                if (storage_file_commit(d->cache, binary))
                {
                    snprintf(d->cache, sizeof(d->cache), "%s", binary);
                    d->persistent = document_cache_commit(d->cached, d->status.title, d->status.end);
                }
            }
            storage_unlock();
        }
    }
    if (result != BOOK_OK && d->cache_result != BOOK_OK) result = d->cache_result;
    if (cancelled(d)) result = BOOK_CANCELLED;
    rt_base_t key = rt_hw_interrupt_disable();
    result_locked(d, result);
    d->status.complete = true;
    rt_hw_interrupt_enable(key);
    rt_kprintf("[document] done: result=%d bytes=%u elapsed=%lu ms\n", result, d->status.end,
        (unsigned long)((uint64_t)(rt_tick_get() - d->started) * 1000 / RT_TICK_PER_SECOND));
}

bool document_worker_step(epd_service_t *service)
{
    if (worker_stopping) return false;
    worker_service = service;
    rt_base_t key = rt_hw_interrupt_disable();
    document_t *d = NULL;
    /* Close cancelled documents before entering another backend instance. */
    for (document_t *p = documents; p; p = p->next)
        if (!p->stopped && cancelled(p)) { d = p; break; }
    if (!d)
        for (document_t *p = documents; p; p = p->next)
            if (p->cache_bad || p->cache_touch || (!p->stopped && (!p->claimed || image_pending(p))))
            { d = p; break; }
    worker_document = d;
    rt_hw_interrupt_enable(key);
    if (!d)
    {
        if (saves) { save_work(); return true; }
        return false;
    }
    if (!cancelled(d) && !d->claimed) { d->claimed = true; convert(d); }
    if (d->cache_bad)
    {
        document_cache_invalidate(d->cached);
        d->cache_bad = d->cache_touch = false;
    }
    else if (d->cache_touch)
    {
        d->cache_touch = false;
        if (!cancelled(d)) document_cache_touch(d->cached);
    }
    image_work(d);
    key = rt_hw_interrupt_disable();
    d->stopped = cancelled(d) || (!image_pending(d) && !d->cache_bad && !d->cache_touch);
    worker_document = NULL;
    rt_hw_interrupt_enable(key);
    return true;
}

void document_worker_shutdown(void)
{
    /* The caller has returned from its last worker_step before draining. */
    rt_base_t key = rt_hw_interrupt_disable();
    worker_stopping = true;
    rt_hw_interrupt_enable(key);
    for (;;)
    {
        key = rt_hw_interrupt_disable();
        document_t *d = documents;
        while (d && d->stopped && !d->cache_bad && d->cancelled) d = d->next;
        worker_document = d;
        rt_hw_interrupt_enable(key);
        if (!d) break;
        d->cancelled = true;
        if (d->cache_bad)
        {
            document_cache_invalidate(d->cached);
            d->cache_bad = false;
        }
        key = rt_hw_interrupt_disable();
        d->cache_touch = false;
        d->stopped = true;
        worker_document = NULL;
        rt_hw_interrupt_enable(key);
    }
    while (saves) save_work();
    worker_service = NULL;
}

void document_worker(epd_service_t *service)
{
    worker_stopping = false;
    worker_service = service;
    while (!epd_service_cancelled(service))
        if (!document_worker_step(service)) epd_service_wait(service, RT_WAITING_FOREVER);
    document_worker_shutdown();
}

document_t *document_open_source(const document_source_t *source, char *error, size_t size)
{
    document_service_process();
    if (!source || !source->convert || !source->path || !source->app_id ||
        !source->cache_path || !source->identity || !*source->app_id || !*source->identity ||
        source->path[0] != '/' || source->cache_path[0] != '/' ||
        strlen(source->path) >= BOOKSHELF_PATH_MAX || strlen(source->app_id) >= 64 ||
        strlen(source->cache_path) + 32 >= STORAGE_PATH_MAX ||
        strlen(source->app_id) + strlen(source->identity) + 2 >= BOOKSHELF_PATH_MAX)
    { snprintf(error, size, "文档输入或缓存身份无效"); return NULL; }
    for (const unsigned char *p = (const unsigned char *)source->app_id; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.'))
        { snprintf(error, size, "应用标识无效"); return NULL; }
    if (worker_stopping)
    {
        if (documents || worker_service || worker_document)
        { snprintf(error, size, "阅读服务正在停止"); return NULL; }
        worker_stopping = false;
    }
    document_t *d = allocate(sizeof(*d));
    if (!d) { snprintf(error, size, "文档会话内存不足"); return NULL; }
    memset(d, 0, sizeof(*d));
    d->input = d->output = -1;
    d->started = rt_tick_get();
    d->generation = d->cache_generation = storage_card_session();
    d->serial = ++sequence; d->format = source->format;
    snprintf(d->file.path, sizeof(d->file.path), "%s", source->path);
    snprintf(d->file.name, sizeof(d->file.name), "%s", source->title ? source->title : "");
    d->file.format = source->format;
    snprintf(d->app_id, sizeof(d->app_id), "%s", source->app_id);
    snprintf(d->directory, sizeof(d->directory), "%s", source->cache_path);
    snprintf(d->identity, sizeof(d->identity), "%s", source->identity);
    d->source = *source;
    d->source.app_id = d->app_id; d->source.cache_path = d->directory;
    d->source.identity = d->identity; d->source.path = d->file.path; d->source.title = d->file.name;
    snprintf(d->status.title, sizeof(d->status.title), "%s", d->file.name);
    d->cached = document_cache_create_source(&d->file, d->app_id, d->directory, d->identity);
    if (d->cached && document_cache_lookup(d->cached, d->status.title, sizeof(d->status.title), &d->end))
    {
        snprintf(d->cache, sizeof(d->cache), "%s", document_cache_binary(d->cached));
        d->status.end = d->pending_end = d->end;
        d->status.publication = 1; d->status.complete = true;
        d->persistent = d->claimed = d->stopped = true; d->cache_touch = true;
    }
    rt_base_t key = rt_hw_interrupt_disable();
    if (worker_stopping)
    {
        rt_hw_interrupt_enable(key);
        document_cache_destroy(d->cached); epd_app_free(d);
        snprintf(error, size, "阅读服务正在停止"); return NULL;
    }
    d->next = documents; documents = d;
    rt_hw_interrupt_enable(key);
    wake_worker();
    return d;
}

document_t *document_open(const bookshelf_item_t *file, char *error, size_t size)
{
    document_service_process();
    document_t *d = allocate(sizeof(*d));
    if (!d) { snprintf(error, size, "文档会话内存不足"); return NULL; }
    memset(d, 0, sizeof(*d));
    d->started = rt_tick_get();
    d->input = d->output = -1;
    d->file = *file;
    d->format = (document_format_t)file->format;
    d->generation = storage_card_session();
    d->serial = ++sequence;
    snprintf(d->status.title, sizeof(d->status.title), "%s", file->name);
    d->cached = document_cache_create(file);
    if (d->cached && document_cache_lookup(d->cached, d->status.title, sizeof(d->status.title), &d->end))
    {
        snprintf(d->cache, sizeof(d->cache), "%s", document_cache_binary(d->cached));
        d->cache_generation = storage_card_session();
        d->status.end = d->pending_end = d->end;
        d->status.publication = 1; d->status.complete = true;
        d->persistent = d->claimed = d->stopped = true;
        d->cache_touch = true;
        rt_kprintf("[document] cache-hit: %s bytes=%u\n", file->path, d->end);
    }
    const char *name = !d->persistent && d->format == DOCUMENT_MOBI ? "bk_mobi" : NULL;
    if (name)
    {
        d->library = epd_app_library_open("books", name);
        if (!d->library)
        {
            snprintf(error, size, "无法加载 %s 解码库", name);
            document_cache_destroy(d->cached); epd_app_free(d); return NULL;
        }
        d->converter = epd_app_library_symbol(d->library, "book_converter");
        if (!d->converter || d->converter->abi != BOOK_CODEC_ABI ||
            d->converter->size < sizeof(*d->converter) || !d->converter->convert) goto incompatible;
    }
    rt_base_t key = rt_hw_interrupt_disable();
    d->next = documents; documents = d;
    rt_hw_interrupt_enable(key);
    wake_worker();
    return d;
incompatible:
    snprintf(error, size, "解码库版本与书架不匹配");
    epd_app_library_close(d->library);
    document_cache_destroy(d->cached);
    epd_app_free(d);
    return NULL;
}

void document_close(document_t *d)
{
    if (!d) return;
    rt_base_t key = rt_hw_interrupt_disable();
    d->cancelled = d->released = true;
    for (document_image_request_t *j = image_requests; j; j = j->next)
        if (j->document == d) j->released = true;
    rt_hw_interrupt_enable(key);
    wake_worker();
}

void document_service_process(void)
{
    image_collect();
    document_t **link = &documents;
    while (*link)
    {
        document_t *d = *link;
        if (!media_valid(d->file.path, d->generation) || !media_valid(d->cache, d->cache_generation))
        {
            d->cancelled = true;
            cache_initialized[STORAGE_SD] = false;
            wake_worker();
        }
        rt_base_t key = rt_hw_interrupt_disable();
        /* Superseded requests that never reached the worker own no backend. */
        if (d->released && !d->claimed && worker_document != d) d->stopped = true;
        bool dispose = d->stopped && d->released && worker_document != d && !image_referenced(d) &&
                       (!worker_service || !d->cache_bad);
        if (dispose) *link = d->next;
        rt_hw_interrupt_enable(key);
        if (!dispose) { link = &d->next; continue; }
        storage_lock(); readers_close(d); storage_unlock();
        while (d->readers) document_reader_destroy(d->readers);
        if (d->input >= 0) { storage_lock(); close(d->input); storage_unlock(); d->input = -1; }
        if (d->cache[0] && !d->persistent && (!d->source.convert || d->temp_owned))
        {
            storage_lock();
            if (media_valid(d->cache, d->cache_generation)) unlink(d->cache);
            storage_unlock();
        }
        if (d->library) { epd_app_library_close(d->library); d->library = NULL; }
        if (d->read_calls || d->write_calls)
            rt_kprintf("[document] io: read=%u/%u write=%u/%u seek=%u open=%u sync=%u\n",
                d->read_calls, d->read_bytes, d->write_calls, d->write_bytes,
                d->seek_calls, d->open_calls, d->sync_calls);
        epd_app_free(d->write_buffer);
        document_cache_destroy(d->cached);
        if (d->source.release_user) d->source.release_user(d->source.user);
        epd_app_free(d);
    }
}

void document_service_shutdown(void)
{
    rt_base_t key = rt_hw_interrupt_disable();
    worker_stopping = true;
    document_save_t *pending = saves;
    saves = NULL;
    rt_hw_interrupt_enable(key);
    while (pending)
    {
        document_save_t *next = pending->next;
        epd_app_free(pending);
        pending = next;
    }
    for (document_t *d = documents; d; d = d->next) document_close(d);
    document_service_process();
    memset(cache_initialized, 0, sizeof(cache_initialized));
}
void document_status(document_t *d, document_status_t *status)
{
    rt_base_t key = rt_hw_interrupt_disable();
    *status = d->status;
    status->stopped = d->stopped;
    rt_hw_interrupt_enable(key);
    if (cancelled(d) && !status->error[0])
    { status->result = BOOK_CANCELLED; snprintf(status->error, sizeof(status->error), "文档读取已取消或存储状态已变化，请重新打开"); }
}
void document_progress(document_t *d, document_progress_t *progress)
{
    rt_base_t key = rt_hw_interrupt_disable();
    progress->end = d->status.end;
    progress->publication = d->status.publication;
    progress->complete = d->status.complete;
    progress->result = d->status.result;
    rt_hw_interrupt_enable(key);
    if (cancelled(d)) progress->result = BOOK_CANCELLED;
}

document_reader_t *document_reader_create(document_t *d)
{
    if (!d || cancelled(d)) return NULL;
    document_reader_t *r = allocate(sizeof(*r));
    if (!r) return NULL;
    memset(r, 0, sizeof(*r));
    r->document = d; r->descriptor = -1;
    r->window = allocate(DOCUMENT_IO_WINDOW);
    storage_lock();
    rt_base_t key = rt_hw_interrupt_disable();
    r->next = d->readers; d->readers = r;
    rt_hw_interrupt_enable(key);
    storage_unlock();
    return r;
}

void document_reader_destroy(document_reader_t *r)
{
    if (!r) return;
    document_t *d = r->document;
    storage_lock();
    if (r->descriptor >= 0) close(r->descriptor);
    rt_base_t key = rt_hw_interrupt_disable();
    document_reader_t **link = &d->readers;
    while (*link && *link != r) link = &(*link)->next;
    if (*link) *link = r->next;
    if (d->default_reader == r) { d->default_reader = NULL; d->input = -1; }
    rt_hw_interrupt_enable(key);
    storage_unlock();
    epd_app_free(r->window); epd_app_free(r);
}

/* storage_lock serializes cursor I/O and the completed-cache name switch. */
static void readers_close(document_t *d)
{
    for (document_reader_t *r = d->readers; r; r = r->next)
    {
        if (r->descriptor >= 0) close(r->descriptor);
        r->descriptor = -1; r->visible = r->window_used = 0;
    }
    d->input = -1;
}

static bool reader_fill(document_reader_t *r, uint32_t offset, void *data, size_t length)
{
    document_t *d = r->document;
    if (r->position != offset)
    {
        ++d->seek_calls;
        if (lseek(r->descriptor, offset, SEEK_SET) != (off_t)offset) return false;
        r->position = offset;
    }
    uint8_t *p = data;
    while (length)
    {
        if (cancelled(d)) return false;
        size_t chunk = length > DOCUMENT_IO_WINDOW ? DOCUMENT_IO_WINDOW : length;
        int n = read(r->descriptor, p, chunk);
        ++d->read_calls;
        if (n <= 0) return false;
        r->position += n; d->read_bytes += n;
        p += n; length -= n;
    }
    return !cancelled(d);
}

static void cache_read_failed(document_t *d)
{
    if (!d->persistent || cancelled(d)) return;
    rt_base_t key = rt_hw_interrupt_disable();
    d->cache_bad = true;
    d->stopped = false;
    result_locked(d, BOOK_IO_ERROR);
    rt_hw_interrupt_enable(key);
    wake_worker();
}

bool document_reader_read(document_reader_t *r, uint32_t offset, void *data, size_t length)
{
    if (!r || (!data && length)) return false;
    document_t *d = r->document;
    document_progress_t status;
    document_progress(d, &status);
    if (status.result != BOOK_OK || offset > status.end || length > status.end - offset) return false;
    if (!length) return true;
    storage_lock();
    bool ok = false;
    if (cancelled(d)) goto done;
    if (r->window && offset >= r->window_start && offset - r->window_start <= r->window_used &&
        length <= r->window_used - (offset - r->window_start))
    {
        memcpy(data, r->window + offset - r->window_start, length);
        ok = true; goto done;
    }
    if (r->descriptor >= 0 && (offset > r->visible || length > r->visible - offset))
    { close(r->descriptor); r->descriptor = -1; }
    if (r->descriptor < 0)
    {
        r->descriptor = open(d->cache, O_RDONLY); ++d->open_calls;
        if (r->descriptor < 0) goto done;
        r->position = 0; r->visible = status.end;
        if (d->default_reader == r) { d->input = r->descriptor; d->input_publication = status.publication; }
    }
    if (r->window && length <= DOCUMENT_IO_WINDOW)
    {
        uint32_t n = r->visible - offset;
        if (n > DOCUMENT_IO_WINDOW) n = DOCUMENT_IO_WINDOW;
        r->window_used = 0;
        if (!reader_fill(r, offset, r->window, n)) goto done;
        r->window_start = offset; r->window_used = n;
        memcpy(data, r->window, length); ok = true;
    }
    else ok = reader_fill(r, offset, data, length);
done:
    if (!ok) cache_read_failed(d);
    storage_unlock();
    return ok;
}

bool document_reader_record(document_reader_t *r, uint32_t offset, document_record_t *record)
{
    if (!r || !document_reader_read(r, offset, record, sizeof(*record))) return false;
    document_progress_t status;
    document_progress(r->document, &status);
    bool ok = status.result == BOOK_OK && offset <= status.end && sizeof(*record) <= status.end - offset &&
           record->length <= status.end - offset - sizeof(*record) &&
           record->kind >= DOCUMENT_TEXT && record->kind <= DOCUMENT_IMAGE &&
           (record->kind == DOCUMENT_IMAGE || record->length <= DOCUMENT_TEXT_LIMIT);
    if (!ok) cache_read_failed(r->document);
    return ok;
}

bool document_reader_image_info(document_reader_t *r, uint32_t offset, document_image_t *image)
{
    document_record_t record;
    if (!image || !document_reader_record(r, offset, &record)) return false;
    if (record.kind != DOCUMENT_IMAGE || record.length < sizeof(*image))
    { cache_read_failed(r->document); return false; }
    if (!document_reader_read(r, offset + sizeof(record), image, sizeof(*image))) return false;
    if (!image->width || !image->height || image->width > DOCUMENT_IMAGE_WIDTH ||
        image->height > DOCUMENT_IMAGE_HEIGHT ||
        record.length != sizeof(*image) + (image->width * image->height + 1) / 2)
    { cache_read_failed(r->document); return false; }
    return true;
}

bool document_read(document_t *d, uint32_t offset, void *data, size_t length)
{
    if (!d->default_reader) d->default_reader = document_reader_create(d);
    return document_reader_read(d->default_reader, offset, data, length);
}
bool document_record(document_t *d, uint32_t offset, document_record_t *record)
{
    if (!d->default_reader) d->default_reader = document_reader_create(d);
    return document_reader_record(d->default_reader, offset, record);
}

static bool image_load(document_reader_t *r, uint32_t offset, unsigned width, unsigned height, document_bitmap_t *bitmap)
{
    document_image_t source;
    if (!document_reader_image_info(r, offset, &source)) return false;
    if (!bitmap_allocate(bitmap, width, height)) return false;
    unsigned bytes = (source.width * source.height + 1) / 2;
    uint8_t *packed = allocate(bytes);
    if (!packed || !document_reader_read(r, offset + sizeof(document_record_t) + sizeof(source), packed, bytes))
    { epd_app_free(packed); epd_app_free(bitmap->memory); bitmap->memory = NULL; return false; }
    for (unsigned y = 0; y < height; ++y)
    {
        if (cancelled(r->document))
        { epd_app_free(packed); epd_app_free(bitmap->memory); memset(bitmap, 0, sizeof(*bitmap)); return false; }
        unsigned pixel = (y * source.height / height) * source.width;
        uint16_t *out = (uint16_t *)(bitmap->image.data + y * bitmap->image.header.stride);
        for (unsigned x = 0; x < width; ++x)
        {
            unsigned sx = pixel + x * source.width / width;
            unsigned nibble = (packed[sx / 2] >> (sx & 1 ? 0 : 4)) & 15;
            out[x] = rgb565(nibble * 17);
        }
    }
    epd_app_free(packed);
    return true;
}

bool document_image_load(document_t *d, uint32_t offset, unsigned width, unsigned height, document_bitmap_t *bitmap)
{
    document_reader_t *r = document_reader_create(d);
    bool ok = r && image_load(r, offset, width, height, bitmap);
    document_reader_destroy(r);
    if (ok) bitmap_clean(bitmap);
    return ok;
}

document_image_request_t *document_image_request(document_t *d, uint32_t offset, unsigned width, unsigned height)
{
    if (!d || cancelled(d) || !width || !height) return NULL;
    document_image_request_t *job = allocate(sizeof(*job));
    if (!job) return NULL;
    memset(job, 0, sizeof(*job));
    job->document = d; job->offset = offset; job->width = width; job->height = height;
    rt_base_t key = rt_hw_interrupt_disable();
    document_image_request_t **tail = &image_requests;
    while (*tail) tail = &(*tail)->next;
    *tail = job;
    d->stopped = false;
    rt_hw_interrupt_enable(key);
    wake_worker();
    return job;
}

int document_image_take(document_image_request_t *job, document_bitmap_t *bitmap)
{
    if (!job || !bitmap || cancelled(job->document)) return -1;
    rt_base_t key = rt_hw_interrupt_disable();
    int result = job->done ? (job->result == BOOK_OK && job->bitmap.memory ? 1 : -1) : 0;
    if (result == 1)
    {
        *bitmap = job->bitmap;
        memset(&job->bitmap, 0, sizeof(job->bitmap));
    }
    rt_hw_interrupt_enable(key);
    if (result == 1) bitmap_clean(bitmap);
    return result;
}

void document_image_cancel(document_image_request_t *job)
{
    if (!job) return;
    rt_base_t key = rt_hw_interrupt_disable();
    job->released = true;
    rt_hw_interrupt_enable(key);
    wake_worker();
}

static bool image_pending(document_t *d)
{
    for (document_image_request_t *j = image_requests; j; j = j->next)
        if (j->document == d && !j->done && !j->released) return true;
    return false;
}

static bool image_referenced(document_t *d)
{
    for (document_image_request_t *j = image_requests; j; j = j->next)
        if (j->document == d) return true;
    return false;
}

static void image_collect(void)
{
    document_image_request_t **link = &image_requests;
    while (*link)
    {
        rt_base_t key = rt_hw_interrupt_disable();
        document_image_request_t *job = *link;
        bool dispose = job->released && !job->active;
        if (dispose) *link = job->next;
        rt_hw_interrupt_enable(key);
        if (!dispose) { link = &job->next; continue; }
        epd_app_free(job->bitmap.memory); epd_app_free(job);
    }
}

static void image_work(document_t *only)
{
    for (;;)
    {
        rt_base_t key = rt_hw_interrupt_disable();
        document_image_request_t *job = image_requests;
        while (job && (job->document != only || job->released || job->done || job->active)) job = job->next;
        if (job) job->active = true;
        rt_hw_interrupt_enable(key);
        if (!job) return;
        document_progress_t progress;
        document_progress(only, &progress);
        /* An incomplete source is allowed to grow; never block its producer. */
        bool waiting = progress.result == BOOK_OK && !progress.complete &&
            (job->offset >= progress.end || sizeof(document_record_t) > progress.end - job->offset);
        bool ok = false;
        if (!waiting && progress.result == BOOK_OK)
        {
            document_reader_t *reader = document_reader_create(only);
            ok = reader && image_load(reader, job->offset, job->width, job->height, &job->bitmap);
            document_reader_destroy(reader);
        }
        key = rt_hw_interrupt_disable();
        job->result = ok ? BOOK_OK : progress.result != BOOK_OK ? progress.result : BOOK_IO_ERROR;
        job->done = !waiting;
        job->active = false;
        rt_hw_interrupt_enable(key);
        if (waiting) return;
    }
}

bool document_save_async(const char *path, const void *header, size_t header_size, const void *data, size_t size)
{
    if (!path || strlen(path) >= sizeof(((document_save_t *)0)->path) ||
        (!header && header_size) || (!data && size) || size > SIZE_MAX - header_size ||
        size + header_size > SIZE_MAX - sizeof(document_save_t)) return false;
    document_save_t *job = allocate(sizeof(*job) + header_size + size);
    if (!job) return false;
    memset(job, 0, sizeof(*job));
    memcpy(job->path, path, strlen(path) + 1);
    job->generation = storage_card_session(); job->length = header_size + size;
    if (header_size) memcpy(job->data, header, header_size);
    if (size) memcpy(job->data + header_size, data, size);
    rt_base_t key = rt_hw_interrupt_disable();
    if (worker_stopping)
    {
        rt_hw_interrupt_enable(key);
        epd_app_free(job);
        return false;
    }
    document_save_t **tail = &saves;
    while (*tail) tail = &(*tail)->next;
    *tail = job;
    rt_hw_interrupt_enable(key);
    wake_worker();
    return true;
}

static void save_work(void)
{
    rt_base_t key = rt_hw_interrupt_disable();
    document_save_t *job = saves;
    if (job) saves = job->next;
    rt_hw_interrupt_enable(key);
    if (!job) return;
    storage_lock();
    if ((!worker_service || !epd_service_cancelled(worker_service)) && media_valid(job->path, job->generation))
    {
        char directory[STORAGE_PATH_MAX];
        memcpy(directory, job->path, strlen(job->path) + 1);
        char *slash = strrchr(directory, '/');
        if (slash) *slash = 0;
        if (slash && storage_mkdirs(directory)) storage_file_replace(job->path, job->data, job->length);
    }
    storage_unlock();
    epd_app_free(job);
}

bool document_index_save(document_t *d, uint32_t layout, const void *data, size_t size)
{
    if (!d || !d->persistent || !d->status.complete || d->status.result != BOOK_OK ||
        cancelled(d) || !size || !data || size > INT32_MAX - sizeof(document_index_header_t)) return false;
    document_index_header_t header = {DOCUMENT_INDEX_MAGIC, 1, document_cache_identity(d->cached),
        layout, d->status.end, (uint32_t)size, hash_bytes(2166136261u, data, size)};
    if (!header.content) return false;
    return document_save_async(document_cache_index(d->cached), &header, sizeof(header), data, size);
}

bool document_index_load(document_t *d, uint32_t layout, void **data, size_t *size)
{
    if (!data || !size) return false;
    *data = NULL; *size = 0;
    if (!d || !d->persistent || !d->status.complete || d->status.result != BOOK_OK || cancelled(d)) return false;
    const char *path = document_cache_index(d->cached);
    document_index_header_t header;
    struct stat st;
    storage_lock();
    int fd = !cancelled(d) && storage_file_recover(path) ? open(path, O_RDONLY) : -1;
    bool ok = fd >= 0 && fstat(fd, &st) == 0 && read(fd, &header, sizeof(header)) == sizeof(header) &&
        header.magic == DOCUMENT_INDEX_MAGIC && header.version == 1 &&
        header.content == document_cache_identity(d->cached) && header.layout == layout &&
        header.end == d->status.end && header.bytes && header.bytes <= INT32_MAX - sizeof(header) &&
        st.st_size == (off_t)(sizeof(header) + header.bytes);
    uint8_t *buffer = ok ? allocate(header.bytes) : NULL;
    if (!buffer) ok = false;
    uint32_t at = 0;
    while (ok && at < header.bytes)
    {
        if (cancelled(d)) { ok = false; break; }
        size_t count = header.bytes - at;
        if (count > DOCUMENT_IO_WINDOW) count = DOCUMENT_IO_WINDOW;
        int n = read(fd, buffer + at, count);
        if (n <= 0) { ok = false; break; }
        at += n;
    }
    if (fd >= 0 && close(fd) != 0) ok = false;
    if (ok) ok = !cancelled(d) && hash_bytes(2166136261u, buffer, header.bytes) == header.checksum;
    storage_unlock();
    if (!ok) { epd_app_free(buffer); return false; }
    *data = buffer; *size = header.bytes;
    return true;
}
