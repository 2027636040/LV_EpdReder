/* SPDX-License-Identifier: Apache-2.0 */
#include "document.h"
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

struct document {
    struct document *next;
    bookshelf_item_t file;
    document_format_t format;
    uint32_t generation, serial, end, link;
    uint32_t cache_generation;
    uint32_t pending_end, input_publication;
    rt_tick_t last_publish, started;
    int output, input, cache_result;
    char cache[80];
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
static uint32_t sequence;
static bool cache_initialized[STORAGE_COUNT];

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
           !media_valid(d->file.path, d->generation) ||
           !media_valid(d->cache, d->cache_generation);
}

static void wake_worker(void)
{
    if (worker_service) epd_service_wake(worker_service);
}

static int cache_failure(document_t *d, const char *operation, int error)
{
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

static int write_all(document_t *d, const void *data, size_t length)
{
    if (cancelled(d)) return BOOK_CANCELLED;
    int result = cache_room(d, length);
    if (result != BOOK_OK) return result;
    const uint8_t *p = data;
    while (length)
    {
        size_t chunk = length > 4096 ? 4096 : length;
        storage_lock();
        if (cancelled(d)) { storage_unlock(); return BOOK_CANCELLED; }
        errno = 0;
        int n = write(d->output, p, chunk);
        int error = errno;
        storage_unlock();
        if (n <= 0) return cache_failure(d, "write", error);
        d->end += n; p += n; length -= n;
        if (cancelled(d)) return BOOK_CANCELLED;
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
    storage_lock();
    int result = BOOK_CANCELLED;
    if (!cancelled(d))
    {
        errno = 0;
        result = fsync(d->output) == 0 ? BOOK_OK : cache_failure(d, "sync", errno);
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
    unsigned stride = RT_ALIGN(width * 2, 4);
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
    char directory[64];
    if (!storage_app_path(directory, sizeof(directory), "books", STORAGE_APP_CACHE, "documents"))
    { storage_unlock(); return false; }
    storage_volume_t volume = !strcmp(storage_path_root(directory), STORAGE_SD_ROOT) ? STORAGE_SD : STORAGE_FLASH;
    d->cache_generation = storage_card_session();
    snprintf(d->cache, sizeof(d->cache), "%s/doc-%08lx.bin", directory, (unsigned long)d->serial);
    if (cancelled(d)) { storage_unlock(); return false; }
    errno = 0;
    bool ok = storage_mkdirs(directory);
    if (!ok) cache_failure(d, "mkdir", errno);
    if (ok && !cache_initialized[volume])
    {
        DIR *dir = opendir(directory);
        struct dirent *entry;
        while (dir && (entry = readdir(dir)) != NULL)
        {
            if (cancelled(d)) { ok = false; break; }
            size_t n = strlen(entry->d_name);
            if (n == 16 && !strncmp(entry->d_name, "doc-", 4) && !strcmp(entry->d_name + 12, ".bin"))
            {
                char path[128];
                snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
                unlink(path);
            }
        }
        if (dir) closedir(dir);
        cache_initialized[volume] = true;
    }
    if (ok && !cancelled(d))
    {
        errno = 0;
        d->output = open(d->cache, O_CREAT | O_TRUNC | O_WRONLY, 0);
        if (d->output < 0) cache_failure(d, "open", errno);
    }
    d->last_publish = rt_tick_get();
    storage_unlock();
    return d->output >= 0;
}

static void convert(document_t *d)
{
    int result = BOOK_IO_ERROR;
    rt_kprintf("[document] begin: %s (%u bytes)\n", d->file.path, d->file.size);
    if (cache_open(d))
    {
        rt_kprintf("[document] cache-open: %s\n", d->cache);
        book_sink_t sink = {d, cancelled, sink_text, sink_paragraph, sink_anchor,
                           sink_image, sink_metadata, sink_link, book_markup_convert};
        if (d->converter)
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
        if (result == BOOK_OK) result = cache_publish(d, true);
        storage_lock();
        errno = 0;
        int closed = close(d->output);
        if (closed != 0 && result == BOOK_OK) result = cache_failure(d, "close", errno);
        d->output = -1;
        storage_unlock();
    }
    if (result != BOOK_OK && d->cache_result != BOOK_OK) result = d->cache_result;
    rt_base_t key = rt_hw_interrupt_disable();
    result_locked(d, result);
    d->status.complete = true;
    rt_hw_interrupt_enable(key);
    rt_kprintf("[document] done: result=%d bytes=%u elapsed=%lu ms\n", result, d->status.end,
        (unsigned long)((uint64_t)(rt_tick_get() - d->started) * 1000 / RT_TICK_PER_SECOND));
}

void document_worker(epd_service_t *service)
{
    worker_service = service;
    while (!epd_service_cancelled(service))
    {
        rt_base_t key = rt_hw_interrupt_disable();
        document_t *d = NULL;
        /* Close cancelled documents before entering another backend instance. */
        for (document_t *p = documents; p; p = p->next)
            if (!p->stopped && cancelled(p)) { d = p; break; }
        if (!d)
            for (document_t *p = documents; p; p = p->next)
                if (!p->stopped && !p->claimed)
                { d = p; break; }
        worker_document = d;
        rt_hw_interrupt_enable(key);
        if (!d) { epd_service_wait(service, RT_WAITING_FOREVER); continue; }
        if (!cancelled(d) && !d->claimed) { d->claimed = true; convert(d); }
        key = rt_hw_interrupt_disable();
        d->stopped = true;
        worker_document = NULL;
        rt_hw_interrupt_enable(key);
    }
    /* A normal UI step may still be in flight when cancellation arrives. */
    for (;;)
    {
        rt_base_t key = rt_hw_interrupt_disable();
        document_t *d = documents;
        while (d && d->stopped) d = d->next;
        worker_document = d;
        rt_hw_interrupt_enable(key);
        if (!d) break;
        d->cancelled = true;
        key = rt_hw_interrupt_disable();
        d->stopped = true;
        worker_document = NULL;
        rt_hw_interrupt_enable(key);
    }
    worker_service = NULL;
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
    const char *name = d->format == DOCUMENT_MOBI ? "bk_mobi" : NULL;
    if (name)
    {
        d->library = epd_app_library_open("books", name);
        if (!d->library) { snprintf(error, size, "无法加载 %s 解码库", name); epd_app_free(d); return NULL; }
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
    epd_app_free(d);
    return NULL;
}

void document_close(document_t *d)
{
    if (!d) return;
    rt_base_t key = rt_hw_interrupt_disable();
    d->cancelled = d->released = true;
    rt_hw_interrupt_enable(key);
    wake_worker();
}

void document_service_process(void)
{
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
        bool dispose = d->stopped && d->released && worker_document != d;
        if (dispose) *link = d->next;
        rt_hw_interrupt_enable(key);
        if (!dispose) { link = &d->next; continue; }
        if (d->input >= 0) { storage_lock(); close(d->input); storage_unlock(); d->input = -1; }
        if (d->cache[0])
        {
            storage_lock();
            if (media_valid(d->cache, d->cache_generation)) unlink(d->cache);
            storage_unlock();
        }
        if (d->library) { epd_app_library_close(d->library); d->library = NULL; }
        epd_app_free(d);
    }
}

void document_service_shutdown(void)
{
    for (document_t *d = documents; d; d = d->next) d->released = true;
    document_service_process();
    memset(cache_initialized, 0, sizeof(cache_initialized));
}
void document_status(document_t *d, document_status_t *status)
{
    rt_base_t key = rt_hw_interrupt_disable();
    *status = d->status;
    status->stopped = d->stopped;
    rt_hw_interrupt_enable(key);
    if ((d->cancelled || !media_valid(d->file.path, d->generation) ||
         !media_valid(d->cache, d->cache_generation)) && !status->error[0])
    { status->result = BOOK_CANCELLED; snprintf(status->error, sizeof(status->error), "存储设备已变化，请返回书架重新打开"); }
}
bool document_read(document_t *d, uint32_t offset, void *data, size_t length)
{
    document_status_t status;
    document_status(d, &status);
    if (cancelled(d) || offset > status.end || length > status.end - offset) return false;
    storage_lock();
    if (cancelled(d)) { storage_unlock(); return false; }
    if (d->input >= 0 && d->input_publication != status.publication)
    { close(d->input); d->input = -1; }
    if (d->input < 0)
    {
        d->input = open(d->cache, O_RDONLY);
        if (d->input >= 0) d->input_publication = status.publication;
    }
    bool ok = d->input >= 0 && lseek(d->input, offset, SEEK_SET) == (off_t)offset;
    uint8_t *p = data;
    while (ok && length)
    {
        if (cancelled(d)) { ok = false; break; }
        int n = read(d->input, p, length);
        if (n <= 0) { ok = false; break; }
        p += n; length -= n;
    }
    storage_unlock();
    return ok;
}
bool document_record(document_t *d, uint32_t offset, document_record_t *record)
{
    if (!document_read(d, offset, record, sizeof(*record))) return false;
    document_status_t status;
    document_status(d, &status);
    return offset <= status.end && sizeof(*record) <= status.end - offset &&
           record->length <= status.end - offset - sizeof(*record) &&
           record->kind >= DOCUMENT_TEXT && record->kind <= DOCUMENT_IMAGE;
}
bool document_image_load(document_t *d, uint32_t offset, unsigned width, unsigned height, document_bitmap_t *bitmap)
{
    document_image_t source;
    document_record_t record;
    if (!document_record(d, offset, &record) || record.kind != DOCUMENT_IMAGE ||
        !document_read(d, offset + sizeof(document_record_t), &source, sizeof(source)) ||
        !source.width || !source.height || source.width > DOCUMENT_IMAGE_WIDTH || source.height > DOCUMENT_IMAGE_HEIGHT ||
        record.length != sizeof(source) + (source.width * source.height + 1) / 2 ||
        !bitmap_allocate(bitmap, width, height)) return false;
    unsigned bytes = (source.width * source.height + 1) / 2;
    uint8_t *packed = allocate(bytes);
    if (!packed || !document_read(d, offset + sizeof(record) + sizeof(source), packed, bytes))
    { epd_app_free(packed); epd_app_free(bitmap->memory); bitmap->memory = NULL; return false; }
    for (unsigned y = 0; y < height; ++y)
    {
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
    bitmap_clean(bitmap);
    return true;
}
