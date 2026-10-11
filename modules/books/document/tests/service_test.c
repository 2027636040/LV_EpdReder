#define _XOPEN_SOURCE 700
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <setjmp.h>
static int mock_open(const char *path, int flags, ...);
static int mock_close(int fd);
static ssize_t mock_read(int fd, void *data, size_t size);
static ssize_t mock_write(int fd, const void *data, size_t size);
static off_t mock_lseek(int fd, off_t offset, int whence);
static int mock_fsync(int fd);
#define open mock_open
#define close mock_close
#define read mock_read
#define write mock_write
#define lseek mock_lseek
#define fsync mock_fsync
#include "../document.c"
#undef open
#undef close
#undef read
#undef write
#undef lseek
#undef fsync

struct epd_service { bool stop; };
static unsigned allocations, modules;
static uint32_t generation = 1;
static bool hotplug, remove_at_lock;
static unsigned syncs;
static bool fail_sync;
static rt_tick_t ticks;
static document_t *publish_on_resume;
static uint32_t publish_end;
static const book_convert_api_t mock_mobi;
static unsigned io_reads, io_writes, io_opens, io_closes, io_seeks;
static size_t read_limit, write_limit;
static int read_fault_after = -1, write_fault_after = -1;
static int read_fault_errno, write_fault_errno;
static int alloc_fail_after = -1;
static size_t alloc_fail_size;
static unsigned alloc_failures, clean_calls, replace_calls, recover_calls, mkdir_calls;
static bool mkdir_ok, replace_fail, recover_fail, replace_to_file;
static char cache_directory[64], replaced_path[STORAGE_PATH_MAX];
static uint8_t replaced_data[32768];
static size_t replaced_size;
static void (*read_hook)(void);
static void (*resume_hook)(void);
static void (*allocation_hook)(size_t);
static jmp_buf worker_idle;
static bool pause_worker_at_idle;

struct document_cache {
    char binary[STORAGE_PATH_MAX], index[STORAGE_PATH_MAX];
    uint32_t identity, end;
};
static struct document_cache cache_template;
static bool cache_hit;
static unsigned cache_objects, converter_calls, cache_invalidations, cache_touches;
static document_t *stopped_bad_document;
static document_image_request_t *stopped_bad_job;
static struct epd_service stopped_bad_service;
static unsigned stopped_bad_stage, stopped_bad_invalidations;

static int mock_open(const char *path, int flags, ...)
{
    ++io_opens;
    if (flags & O_CREAT)
    {
        va_list args;
        va_start(args, flags);
        mode_t mode = (mode_t)va_arg(args, int);
        va_end(args);
        return open(path, flags, mode);
    }
    return open(path, flags);
}
static int mock_close(int fd) { ++io_closes; return close(fd); }
static off_t mock_lseek(int fd, off_t offset, int whence)
{ ++io_seeks; return lseek(fd, offset, whence); }
static ssize_t mock_read(int fd, void *data, size_t size)
{
    ++io_reads;
    if (read_fault_after == 0) { errno = read_fault_errno; return errno ? -1 : 0; }
    if (read_fault_after > 0) --read_fault_after;
    if (read_limit && size > read_limit) size = read_limit;
    ssize_t n = read(fd, data, size);
    if (read_hook) { void (*hook)(void) = read_hook; read_hook = NULL; hook(); }
    return n;
}
static ssize_t mock_write(int fd, const void *data, size_t size)
{
    ++io_writes;
    if (write_fault_after == 0) { errno = write_fault_errno; return errno ? -1 : 0; }
    if (write_fault_after > 0) --write_fault_after;
    if (write_limit && size > write_limit) size = write_limit;
    return write(fd, data, size);
}

static bool allocation_fails(size_t size)
{
    if ((alloc_fail_size && size == alloc_fail_size) || alloc_fail_after == 0)
    { ++alloc_failures; return true; }
    if (alloc_fail_after > 0) --alloc_fail_after;
    return false;
}

void *epd_app_alloc(size_t n, int region)
{
    (void)region;
    if (allocation_fails(n)) return NULL;
    void *p = malloc(n);
    if (p)
    {
        ++allocations;
        if (allocation_hook)
        { void (*hook)(size_t) = allocation_hook; allocation_hook = NULL; hook(n); }
    }
    return p;
}
void *epd_app_realloc(void *p, size_t n, int region)
{ (void)region; if (!p) return epd_app_alloc(n, region); if (allocation_fails(n)) return NULL; return realloc(p, n); }
void epd_app_free(void *p) { if (p) { assert(allocations); --allocations; free(p); } }
rt_tick_t rt_tick_get(void) { return ticks; }
rt_tick_t rt_tick_from_millisecond(int ms) { return ms; }
static int mock_fsync(int fd)
{ assert(fd >= 0); ++syncs; if (fail_sync) { errno = EIO; return -1; } return fsync(fd); }
rt_base_t rt_hw_interrupt_disable(void) { return 0; }
void rt_hw_interrupt_enable(rt_base_t level)
{
    (void)level;
    if (publish_on_resume)
    {
        publish_on_resume->status.end = publish_end;
        ++publish_on_resume->status.publication;
        publish_on_resume = NULL;
    }
    if (resume_hook) { void (*hook)(void) = resume_hook; resume_hook = NULL; hook(); }
}
const char *storage_path_root(const char *p)
{ return !strncmp(p, "/sdcard", 7) || !strncmp(p, "/tmp/books_sd_", 14) ? STORAGE_SD_ROOT : STORAGE_FLASH_ROOT; }
bool storage_path_available(const char *p) { return !hotplug || strcmp(storage_path_root(p), STORAGE_SD_ROOT); }
bool storage_available(storage_volume_t v) { return v != STORAGE_SD || !hotplug; }
uint32_t storage_revision(void) { return generation; }
uint32_t storage_card_session(void) { return generation; }
bool storage_app_path(char *out, size_t size, const char *id, storage_app_area_t area, const char *relative)
{
    assert(!strcmp(id, "books") && area == STORAGE_APP_CACHE);
    if (cache_directory[0])
    {
        int n = snprintf(out, size, "%s", cache_directory);
        return n > 0 && (size_t)n < size;
    }
    int n = snprintf(out, size, "/flash/cache/books/%s", relative);
    return n > 0 && (size_t)n < size;
}
bool storage_changing(void) { return hotplug; }
void storage_lock(void)
{ if (remove_at_lock) { remove_at_lock = false; ++generation; hotplug = true; } }
void storage_unlock(void) {}
bool storage_mkdirs(const char *p) { assert(p); ++mkdir_calls; return mkdir_ok; }
bool storage_info(storage_volume_t v, storage_info_t *info) { (void)v; memset(info, 0, sizeof(*info)); return false; }
bool storage_file_signature(const char *p, uint32_t *out) { (void)p; *out = 1; return true; }
bool storage_file_replace(const char *p, const void *data, size_t n)
{
    ++replace_calls;
    assert(n <= sizeof(replaced_data));
    snprintf(replaced_path, sizeof(replaced_path), "%s", p);
    memcpy(replaced_data, data, n); replaced_size = n;
    if (replace_fail) return false;
    if (replace_to_file)
    {
        int fd = open(p, O_CREAT | O_TRUNC | O_WRONLY, 0600);
        assert(fd >= 0 && write(fd, data, n) == (ssize_t)n && close(fd) == 0);
    }
    return true;
}
bool storage_file_recover(const char *p) { assert(p); ++recover_calls; return !recover_fail; }
bool storage_file_commit(const char *source, const char *target) { return rename(source, target) == 0; }
document_cache_t *document_cache_create(const bookshelf_item_t *file)
{
    assert(file);
    if (!cache_hit) return NULL;
    document_cache_t *cache = epd_app_alloc(sizeof(*cache), EPD_APP_PSRAM);
    if (cache) { *cache = cache_template; ++cache_objects; }
    return cache;
}
const char *document_cache_base(const document_cache_t *cache) { return cache ? cache->binary : ""; }
document_cache_t *document_cache_create_source(const bookshelf_item_t *file, const char *app,
                                              const char *directory, const char *identity)
{ (void)app; (void)directory; (void)identity; return document_cache_create(file); }
const char *document_cache_binary(const document_cache_t *cache) { return cache ? cache->binary : ""; }
const char *document_cache_meta(const document_cache_t *cache) { return cache ? cache->binary : ""; }
const char *document_cache_index(const document_cache_t *cache) { return cache ? cache->index : ""; }
uint32_t document_cache_identity(const document_cache_t *cache) { return cache ? cache->identity : 0; }
bool document_cache_lookup(document_cache_t *cache, char *title, size_t size, uint32_t *end)
{
    if (!cache_hit || !cache) return false;
    snprintf(title, size, "Cached title"); *end = cache->end; return true;
}
bool document_cache_prepare(document_cache_t *cache) { (void)cache; return false; }
bool document_cache_invalidate(document_cache_t *cache)
{
    assert(cache && worker_service && worker_document);
    assert(worker_document->cached == cache && worker_document->cache_bad);
    if (worker_document == stopped_bad_document)
    {
        assert(stopped_bad_stage == 2 && worker_stopping && worker_document->stopped);
        assert(worker_document->cancelled && worker_document->released);
        stopped_bad_stage = 3;
    }
    else assert(!worker_document->stopped);
    ++cache_invalidations; return false;
}
bool document_cache_touch(document_cache_t *cache)
{ assert(cache && worker_service); ++cache_touches; return false; }
bool document_cache_observe_begin(document_cache_t *cache) { (void)cache; return false; }
void document_cache_observe_end(document_cache_t *cache) { (void)cache; }
bool document_cache_commit(document_cache_t *cache, const char *title, uint32_t end)
{ (void)cache; (void)title; (void)end; return false; }
bool document_cache_reclaim(document_cache_t *cache) { (void)cache; return false; }
void document_cache_destroy(document_cache_t *cache)
{ if (cache) { assert(cache_objects); --cache_objects; epd_app_free(cache); } }
void *epd_app_library_open(const char *app, const char *name)
{ assert(!strcmp(app, "books") && !strcmp(name, "bk_mobi")); ++modules; return (void *)1; }
void *epd_app_library_symbol(void *module, const char *name)
{ assert(module && !strcmp(name, "book_converter")); return (void *)&mock_mobi; }
void epd_app_library_close(void *module) { assert(module && modules); --modules; }
void epd_app_clean_draw_buffer(lv_draw_buf_t *buf) { assert(buf->data && buf->data_size); ++clean_calls; }
bool epd_service_cancelled(epd_service_t *s) { return s->stop; }
void epd_service_wake(epd_service_t *s) { (void)s; }
uint32_t epd_service_wait(epd_service_t *s, rt_int32_t ticks)
{
    (void)ticks;
    document_service_process();
    if (pause_worker_at_idle) { pause_worker_at_idle = false; longjmp(worker_idle, 1); }
    s->stop = true;
    return 0;
}
book_format_t book_format_detect(const char *path, bool directory) { (void)path; (void)directory; return BOOK_FORMAT_MOBI; }
const char *book_format_name(book_format_t f) { (void)f; return "MOBI"; }
int book_markup_begin(const book_allocator_t *a) { (void)a; return BOOK_OK; }
void book_markup_end(void) {}
int book_markup_convert(const book_sink_t *s, book_stream_t *in, const char *base, const book_resources_t *resources)
{ (void)s; (void)in; (void)base; (void)resources; return BOOK_OK; }
int book_format_convert(const char *p, const book_sink_t *s, const book_allocator_t *a, char *error, size_t size)
{ (void)p; (void)s; (void)a; (void)error; (void)size; ++converter_calls; return BOOK_UNSUPPORTED; }
int document_image_decode(book_stream_t *s, bool (*cb)(void *), void *u, uint8_t **pixels, unsigned *w, unsigned *h)
{ (void)s; (void)cb; (void)u; (void)pixels; (void)w; (void)h; return BOOK_UNSUPPORTED; }
static const book_convert_api_t mock_mobi = {
    BOOK_CODEC_ABI, sizeof(book_convert_api_t), book_format_convert
};

static document_t *open_mobi(const char *path)
{
    bookshelf_item_t file = {0}; char error[96] = {0};
    snprintf(file.path, sizeof(file.path), "%s", path); file.format = BOOK_FORMAT_MOBI;
    document_t *d = document_open(&file, error, sizeof(error));
    assert(d && !error[0]); return d;
}
static void assert_empty(void)
{
    assert(!documents && !worker_document && !modules && !allocations);
    assert(!image_requests && !saves && !cache_objects && !worker_service);
}

static void test_superseded_requests(void)
{
    for (unsigned i = 0; i < 100; ++i)
    {
        document_t *d = open_mobi("/flash/pending.mobi");
        document_close(d); document_service_process(); assert_empty();
    }
}
static void test_worker_owned_release(void)
{
    document_t *d = open_mobi("/flash/active.mobi");
    d->claimed = true;
    worker_document = d;
    document_close(d);
    document_service_process();
    assert(documents == d && modules == 1);
    d->stopped = true;
    document_service_process();
    assert(documents == d && modules == 1);
    worker_document = NULL;
    document_service_process();
    assert_empty();
}
static void test_hotplug_cancels_source(void)
{
    document_t *d = open_mobi("/sdcard/book.mobi");
    ++generation; hotplug = true; document_service_process();
    assert(d->cancelled);
    document_close(d);
    struct epd_service service = {false};
    document_worker(&service); document_service_process(); hotplug = false; assert_empty();
}
static void test_cache_hotplug_inside_lock(void)
{
    document_t d = {0};
    strcpy(d.file.path, "/flash/source.epub");
    strcpy(d.cache, "/sdcard/.epd_reader/cache/doc.bin");
    d.generation = d.cache_generation = generation;
    d.output = d.input = -1;
    assert(!cancelled(&d));
    remove_at_lock = true;
    assert(write_all(&d, "x", 1) == BOOK_CANCELLED && d.end == 0);
    assert(cancelled(&d));
    hotplug = false;
    assert(cancelled(&d));
    d.cache_generation = generation;
    d.status.end = 1;
    remove_at_lock = true;
    char byte;
    assert(!document_read(&d, 0, &byte, 1) && d.input == -1);
    hotplug = false;
    while (d.readers) document_reader_destroy(d.readers);
}
static void test_publish_after_sync(void)
{
    document_t d = {0};
    strcpy(d.file.path, "/flash/source.epub");
    strcpy(d.cache, "/tmp/books_lfs_XXXXXX");
    d.output = mkstemp(d.cache); assert(d.output >= 0);
    d.input = -1;
    d.generation = d.cache_generation = generation;
    unsigned before = syncs;
    assert(record_write(&d, DOCUMENT_TEXT, BOOK_STYLE_BOLD, "first", 5) == BOOK_OK);
    assert(d.status.end == 0 && d.pending_end == sizeof(document_record_t) + 5 && syncs == before);
    assert(cache_publish(&d, true) == BOOK_OK && syncs == before + 1 && d.status.publication == 1);
    document_record_t record;
    assert(document_record(&d, 0, &record) && record.length == 5 && d.input_publication == 1);
    unsigned second = d.end;
    assert(record_write(&d, DOCUMENT_TEXT, 0, "next", 4) == BOOK_OK);
    assert(!document_record(&d, second, &record));
    fail_sync = true;
    assert(cache_publish(&d, true) == BOOK_IO_ERROR && d.status.end == second && d.status.publication == 1);
    fail_sync = false;
    assert(cache_publish(&d, true) == BOOK_OK);
    assert(document_record(&d, second, &record) && record.length == 4 && d.input_publication == 2);
    while (d.readers) document_reader_destroy(d.readers);
    close(d.output); unlink(d.cache);
}
static void test_record_publication_race(void)
{
    document_t d = {0};
    strcpy(d.file.path, "/flash/source.epub");
    strcpy(d.cache, "/tmp/books_record_XXXXXX");
    d.output = mkstemp(d.cache); assert(d.output >= 0);
    d.input = -1;
    d.generation = d.cache_generation = generation;
    document_record_t malformed = {DOCUMENT_TEXT, 1024, 0, 0}, record;
    assert(write_all(&d, &malformed, sizeof(malformed)) == BOOK_OK);
    publish_on_resume = &d; publish_end = sizeof(malformed);
    /* Publish after the reader's first snapshot. An incomplete payload must
     * remain invalid even when the old boundary is before the new header. */
    assert(!document_record(&d, 0, &record));
    assert(!publish_on_resume && d.status.end == sizeof(malformed));
    assert(!document_record(&d, 0, &record));
    assert(!document_record(&d, d.status.end, &record));
    assert(!document_record(&d, UINT32_MAX, &record));
    d.status.end = sizeof(malformed) - 1;
    assert(!document_record(&d, 0, &record));
    while (d.readers) document_reader_destroy(d.readers);
    close(d.output); unlink(d.cache);
}
static void test_cache_capacity(void)
{
    document_t d = {0};
    strcpy(d.file.path, "/flash/source.epub");
    strcpy(d.cache, "/tmp/books_capacity_XXXXXX");
    d.output = mkstemp(d.cache); assert(d.output >= 0);
    d.input = -1;
    char text[1024]; memset(text, 'x', sizeof(text));
    for (unsigned i = 0; i < 2300; ++i)
        assert(record_write(&d, DOCUMENT_TEXT, 0, text, sizeof(text)) == BOOK_OK);
    assert(d.end > 2u * 1024u * 1024u && cache_publish(&d, true) == BOOK_OK);
    assert(d.status.end == d.end);
    close(d.output); unlink(d.cache);
    d.end = 64u * 1024u * 1024u;
    assert(cache_room(&d, 1024) == BOOK_OK);
    d.end = INT32_MAX;
    assert(cache_room(&d, 1) == BOOK_UNSUPPORTED);
    memset(&d, 0, sizeof(d));
    assert(cache_failure(&d, "write", ENOSPC) == BOOK_NO_SPACE);
    strcpy(d.work_error, "generic converter error");
    result_locked(&d, BOOK_NO_SPACE);
    assert(!strcmp(d.status.error, d.cache_error));
}

static void fixture_open(document_t *d, bool buffered)
{
    memset(d, 0, sizeof(*d));
    strcpy(d->file.path, "/flash/source.epub");
    strcpy(d->cache, "/tmp/books_service_XXXXXX");
    d->output = mkstemp(d->cache); assert(d->output >= 0);
    d->input = -1;
    d->generation = d->cache_generation = generation;
    if (buffered) { d->write_buffer = allocate(DOCUMENT_IO_WINDOW); assert(d->write_buffer); }
}

static void fixture_close(document_t *d)
{
    while (d->readers) document_reader_destroy(d->readers);
    if (d->output >= 0) assert(close(d->output) == 0);
    assert(unlink(d->cache) == 0);
    epd_app_free(d->write_buffer);
}

static void worker_run(void)
{
    struct epd_service service = {false};
    /* Pump a live service to its idle wait. Real termination is exercised by
     * calling document_worker directly, including its admission latch. */
    pause_worker_at_idle = true;
    if (!setjmp(worker_idle)) document_worker(&service);
    assert(!pause_worker_at_idle && !worker_document && !worker_stopping);
    worker_service = NULL;
    document_service_process();
}

static void file_contents(int fd, void *data, size_t size)
{
    struct stat st;
    assert(fstat(fd, &st) == 0 && st.st_size == (off_t)size);
    assert(lseek(fd, 0, SEEK_SET) == 0);
    assert(read(fd, data, size) == (ssize_t)size);
}

static void test_small_write_bytes(void)
{
    uint8_t expected[32768], actual[sizeof(expected)];
    unsigned writes[2], publications[2];
    for (unsigned buffered = 0; buffered < 2; ++buffered)
    {
        document_t d;
        fixture_open(&d, buffered != 0);
        size_t used = 0;
        unsigned before = io_writes;
        for (unsigned i = 0; i < 900; ++i)
        {
            uint8_t payload[11];
            size_t n = i % 12;
            for (size_t j = 0; j < n; ++j) payload[j] = (uint8_t)(i + j);
            d.link = i % 5 ? 0 : i + 1;
            document_record_t record = {n ? DOCUMENT_TEXT : DOCUMENT_PARAGRAPH,
                                        (uint32_t)n, i % 4, d.link};
            assert(used + sizeof(record) + n <= sizeof(expected));
            memcpy(expected + used, &record, sizeof(record)); used += sizeof(record);
            memcpy(expected + used, payload, n); used += n;
            assert(record_write(&d, record.kind, record.style, payload, n) == BOOK_OK);
            assert(d.end == used);
        }
        assert(cache_publish(&d, true) == BOOK_OK && d.status.end == used);
        writes[buffered] = io_writes - before;
        assert(d.write_calls == writes[buffered] && d.write_bytes == used);
        publications[buffered] = d.status.publication;
        file_contents(d.output, actual, used);
        assert(!memcmp(expected, actual, used));
        fixture_close(&d);
    }
    assert(writes[1] < writes[0] / 16 && publications[0] == publications[1]);
    printf("small writes: direct=%u buffered=%u, identical bytes and publication count\n", writes[0], writes[1]);
}

static void test_incomplete_and_unsynced_visibility(void)
{
    document_t d;
    fixture_open(&d, true);
    document_record_t header = {DOCUMENT_TEXT, 4, 0, 0}, result;
    struct stat st;
    assert(write_all(&d, &header, sizeof(header)) == BOOK_OK);
    assert(write_all(&d, "ab", 2) == BOOK_OK);
    assert(d.end == sizeof(header) + 2 && d.pending_end == 0);
    assert(fstat(d.output, &st) == 0 && st.st_size == 0);
    assert(write_flush(&d) == BOOK_OK);
    unsigned before = syncs, opens = io_opens;
    assert(cache_publish(&d, true) == BOOK_OK && syncs == before);
    assert(!document_record(&d, 0, &result) && io_opens == opens);
    assert(write_all(&d, "cd", 2) == BOOK_OK && record_complete(&d) == BOOK_OK);
    fail_sync = true;
    assert(cache_publish(&d, true) == BOOK_IO_ERROR);
    assert(d.status.end == 0 && d.status.publication == 0);
    assert(fstat(d.output, &st) == 0 && st.st_size == (off_t)d.end);
    assert(!document_record(&d, 0, &result) && io_opens == opens);
    fail_sync = false;
    assert(cache_publish(&d, true) == BOOK_OK);
    assert(document_record(&d, 0, &result) && result.length == 4);
    char payload[4];
    assert(document_read(&d, sizeof(header), payload, sizeof(payload)) && !memcmp(payload, "abcd", 4));
    fixture_close(&d);
}

static void test_short_io(void)
{
    document_t d;
    fixture_open(&d, true);
    uint8_t payload[127], actual[sizeof(payload)];
    for (size_t i = 0; i < sizeof(payload); ++i) payload[i] = (uint8_t)i;
    write_limit = 3;
    assert(record_write(&d, DOCUMENT_TEXT, 0, payload, sizeof(payload)) == BOOK_OK);
    assert(cache_publish(&d, true) == BOOK_OK);
    assert(d.write_calls == (sizeof(document_record_t) + sizeof(payload) + 2) / 3);
    write_limit = 0;
    read_limit = 5;
    document_record_t record;
    assert(document_record(&d, 0, &record) && record.length == sizeof(payload));
    assert(document_read(&d, sizeof(record), actual, sizeof(actual)) && !memcmp(payload, actual, sizeof(payload)));
    assert(d.read_calls == (d.status.end + 4) / 5);
    read_limit = 0;
    fixture_close(&d);
}

static void test_partial_write_failure(void)
{
    for (unsigned buffered = 0; buffered < 2; ++buffered)
        for (unsigned error = 0; error < 2; ++error)
        {
            document_t d;
            fixture_open(&d, buffered != 0);
            assert(record_write(&d, DOCUMENT_TEXT, 0, "old", 3) == BOOK_OK);
            assert(cache_publish(&d, true) == BOOK_OK);
            uint32_t visible = d.status.end, publication = d.status.publication;
            unsigned before = syncs;
            write_limit = 2; write_fault_after = 2; write_fault_errno = error ? EIO : 0;
            int result = record_write(&d, DOCUMENT_TEXT, 0, "new payload", 11);
            if (buffered) { assert(result == BOOK_OK); result = cache_publish(&d, true); }
            assert(result == BOOK_IO_ERROR && syncs == before);
            assert(d.status.end == visible && d.status.publication == publication && d.write_used == 0);
            document_record_t record;
            assert(!document_record(&d, visible, &record));
            struct stat st;
            assert(fstat(d.output, &st) == 0 && st.st_size == (off_t)visible + 4);
            write_fault_after = -1; write_limit = 0;
            fixture_close(&d);
        }
}

static void test_partial_read_failure(void)
{
    for (unsigned window = 0; window < 2; ++window)
        for (unsigned error = 0; error < 2; ++error)
        {
            document_t d;
            fixture_open(&d, false);
            assert(record_write(&d, DOCUMENT_TEXT, 0, "payload", 7) == BOOK_OK);
            assert(cache_publish(&d, true) == BOOK_OK);
            alloc_fail_size = window ? 0 : DOCUMENT_IO_WINDOW;
            document_reader_t *r = document_reader_create(&d); assert(r);
            alloc_fail_size = 0;
            read_limit = 3; read_fault_after = 1; read_fault_errno = error ? EIO : 0;
            document_record_t record;
            assert(!document_reader_record(r, 0, &record) && r->window_used == 0);
            read_fault_after = -1; read_limit = 0;
            assert(document_reader_record(r, 0, &record) && record.length == 7);
            char payload[7];
            assert(document_reader_read(r, sizeof(record), payload, sizeof(payload)) && !memcmp(payload, "payload", 7));
            fixture_close(&d);
        }
}

static void test_independent_readers(void)
{
    document_t d;
    fixture_open(&d, false);
    uint8_t payload[DOCUMENT_IO_WINDOW * 3], actual[128];
    for (size_t i = 0; i < sizeof(payload); ++i) payload[i] = (uint8_t)(i * 7 + i / 251);
    assert(write_all(&d, payload, sizeof(payload)) == BOOK_OK);
    assert(record_complete(&d) == BOOK_OK && cache_publish(&d, true) == BOOK_OK);
    document_reader_t *a = document_reader_create(&d), *b = document_reader_create(&d);
    assert(a && b && a->window && b->window && a->window != b->window);
    unsigned reads = io_reads, opens = io_opens, seeks = io_seeks;
    assert(document_reader_read(b, DOCUMENT_IO_WINDOW, actual, sizeof(actual)));
    assert(!memcmp(actual, payload + DOCUMENT_IO_WINDOW, sizeof(actual)));
    for (size_t offset = 0; offset < sizeof(payload); offset += sizeof(actual))
    {
        assert(document_reader_read(a, offset, actual, sizeof(actual)));
        assert(!memcmp(actual, payload + offset, sizeof(actual)));
        assert(document_reader_read(b, DOCUMENT_IO_WINDOW + 37, actual, sizeof(actual)));
        assert(!memcmp(actual, payload + DOCUMENT_IO_WINDOW + 37, sizeof(actual)));
    }
    assert(io_reads - reads == 4 && io_opens - opens == 2 && io_seeks - seeks == 1);
    assert(a->descriptor != b->descriptor && a->position == sizeof(payload));
    assert(b->position == DOCUMENT_IO_WINDOW * 2);
    assert(d.read_calls == 4 && d.open_calls == 2 && d.seek_calls == 1);
    fixture_close(&d);
}

static void test_publication_preserves_old_readers(void)
{
    document_t d;
    fixture_open(&d, true);
    assert(record_write(&d, DOCUMENT_TEXT, 0, "first", 5) == BOOK_OK && cache_publish(&d, true) == BOOK_OK);
    document_reader_t *a = document_reader_create(&d), *b = document_reader_create(&d);
    document_record_t record;
    assert(a && b && document_reader_record(a, 0, &record) && document_reader_record(b, 0, &record));
    uint32_t old_end = d.status.end;
    int a_fd = a->descriptor, b_fd = b->descriptor;
    assert(record_write(&d, DOCUMENT_TEXT, 0, "second", 6) == BOOK_OK && cache_publish(&d, true) == BOOK_OK);
    unsigned opens = io_opens, closes = io_closes, reads = io_reads;
    assert(document_reader_record(a, 0, &record) && document_reader_record(b, 0, &record));
    assert(io_opens == opens && io_closes == closes && io_reads == reads);
    assert(a->descriptor == a_fd && b->descriptor == b_fd && a->visible == old_end && b->visible == old_end);
    /* Force an old-region cache miss: publication alone must not reopen it. */
    a->window_used = 0;
    assert(document_reader_record(a, 0, &record));
    assert(io_opens == opens && io_closes == closes && a->visible == old_end);
    assert(document_reader_record(a, old_end, &record) && record.length == 6);
    assert(io_opens == opens + 1 && io_closes == closes + 1 && a->visible == d.status.end);
    assert(b->visible == old_end && b->descriptor == b_fd);
    assert(document_reader_record(b, old_end, &record));
    assert(io_opens == opens + 2 && io_closes == closes + 2);
    fixture_close(&d);
}

static void test_optional_window_failure(void)
{
    document_t d = {0};
    strcpy(cache_directory, "/tmp/books_service_dir_XXXXXX"); assert(mkdtemp(cache_directory));
    strcpy(d.file.path, "/flash/source.epub");
    d.input = d.output = -1; d.generation = generation; d.serial = ++sequence;
    mkdir_ok = true;
    alloc_fail_size = DOCUMENT_IO_WINDOW;
    unsigned failures = alloc_failures;
    assert(cache_open(&d) && !d.write_buffer && alloc_failures == failures + 1);
    /* The target DFS ignores mode 0; Linux enforces it on the reader open. */
    assert(fchmod(d.output, S_IRUSR | S_IWUSR) == 0);
    assert(record_write(&d, DOCUMENT_TEXT, 0, "fallback", 8) == BOOK_OK && cache_publish(&d, true) == BOOK_OK);
    document_reader_t *r = document_reader_create(&d);
    assert(r && !r->window && alloc_failures == failures + 2);
    alloc_fail_size = 0;
    unsigned reads = io_reads, seeks = io_seeks, opens = io_opens;
    document_record_t record;
    char actual[8];
    assert(document_reader_record(r, 0, &record) && record.length == 8);
    assert(document_reader_read(r, sizeof(record), actual, sizeof(actual)) && !memcmp(actual, "fallback", 8));
    assert(io_reads == reads + 2 && io_seeks == seeks && io_opens == opens + 1);
    alloc_fail_after = 0;
    assert(!document_reader_create(&d));
    alloc_fail_after = -1;
    fixture_close(&d);
    assert(rmdir(cache_directory) == 0); cache_directory[0] = 0; mkdir_ok = false;
}

static document_t *image_document(bool published)
{
    document_t *d = open_mobi("/flash/image.mobi");
    strcpy(d->cache, "/tmp/books_image_XXXXXX");
    d->output = mkstemp(d->cache); assert(d->output >= 0);
    d->cache_generation = generation;
    d->claimed = true; d->stopped = true;
    if (published)
    {
        document_image_t dimensions = {2, 2};
        uint8_t payload[sizeof(dimensions) + 2];
        memcpy(payload, &dimensions, sizeof(dimensions));
        payload[sizeof(dimensions)] = 0x05; payload[sizeof(dimensions) + 1] = 0xaf;
        assert(record_write(d, DOCUMENT_IMAGE, 0, payload, sizeof(payload)) == BOOK_OK);
        assert(cache_publish(d, true) == BOOK_OK);
        assert(close(d->output) == 0); d->output = -1;
        d->status.complete = true;
    }
    return d;
}

static void test_image_pending_worker_take(void)
{
    document_t *d = image_document(false);
    document_image_request_t *job = document_image_request(d, 0, 2, 2);
    document_bitmap_t bitmap = {0};
    unsigned reads = io_reads, cleans = clean_calls;
    assert(job && document_image_take(job, &bitmap) == 0 && !bitmap.memory);
    image_work(d);
    assert(!job->done && !job->active && document_image_take(job, &bitmap) == 0);
    assert(io_reads == reads && clean_calls == cleans);
    document_image_t dimensions = {2, 2};
    uint8_t payload[sizeof(dimensions) + 2];
    memcpy(payload, &dimensions, sizeof(dimensions));
    payload[sizeof(dimensions)] = 0x05; payload[sizeof(dimensions) + 1] = 0xaf;
    assert(record_write(d, DOCUMENT_IMAGE, 0, payload, sizeof(payload)) == BOOK_OK);
    assert(cache_publish(d, true) == BOOK_OK);
    assert(close(d->output) == 0); d->output = -1; d->status.complete = true;
    document_reader_t *ui_reader = document_reader_create(d);
    document_record_t record;
    assert(ui_reader && document_reader_record(ui_reader, 0, &record));
    int ui_fd = ui_reader->descriptor;
    uint32_t position = ui_reader->position;
    worker_run();
    assert(job->done && !job->active && d->stopped && clean_calls == cleans);
    assert(ui_reader->descriptor == ui_fd && ui_reader->position == position);
    assert(document_image_take(job, &bitmap) == 1 && clean_calls == cleans + 1);
    assert(bitmap.memory && !job->bitmap.memory && bitmap.image.header.w == 2 && bitmap.image.header.h == 2);
    assert(bitmap.image.header.stride == 4 && bitmap.image.data_size == 8);
    const uint16_t expected[] = {0x0000, 0x52aa, 0xad55, 0xffff};
    assert(!memcmp(bitmap.image.data, expected, sizeof(expected)));
    document_bitmap_t second = {0};
    assert(document_image_take(job, &second) == -1 && !second.memory && clean_calls == cleans + 1);
    document_image_cancel(job); document_service_process();
    assert(!image_requests && bitmap.memory);
    document_reader_destroy(ui_reader);
    document_close(d); document_service_process();
    assert(!documents && allocations == 1);
    epd_app_free(bitmap.memory); assert_empty();
}

static void test_image_cancel_pending_and_ready(void)
{
    for (unsigned ready = 0; ready < 2; ++ready)
    {
        document_t *d = image_document(true);
        document_image_request_t *job = document_image_request(d, 0, 2, 2); assert(job);
        unsigned reads = io_reads;
        if (ready) { worker_run(); assert(job->done && job->bitmap.memory); }
        document_image_cancel(job); document_service_process();
        assert(!image_requests);
        if (!ready) assert(io_reads == reads);
        document_close(d); worker_run(); assert_empty();
    }
}

static document_t *inflight_document;
static document_image_request_t *inflight_job;
static bool inflight_close, inflight_seen;
static void cancel_during_image_read(void)
{
    assert(inflight_job->active && worker_document == inflight_document);
    unsigned owned = allocations;
    if (inflight_close) document_close(inflight_document);
    else document_image_cancel(inflight_job);
    document_service_process();
    assert(documents == inflight_document && image_requests == inflight_job);
    assert(allocations == owned && modules == 1 && !inflight_document->stopped);
    inflight_seen = true;
}

static void test_image_inflight_lifetime(void)
{
    for (unsigned close_document = 0; close_document < 2; ++close_document)
    {
        inflight_document = image_document(true);
        inflight_job = document_image_request(inflight_document, 0, 2, 2); assert(inflight_job);
        inflight_close = close_document != 0; inflight_seen = false;
        read_hook = cancel_during_image_read;
        worker_run();
        assert(inflight_seen && !read_hook && !image_requests);
        if (!close_document) { document_close(inflight_document); worker_run(); }
        inflight_document = NULL; inflight_job = NULL;
        assert_empty();
    }
}

static document_image_request_t *race_first, *race_second;
static document_t *race_document;
static unsigned race_done_boundaries;
static void request_at_image_work_return(void)
{
    if (!race_first->done || ++race_done_boundaries < 2)
    { resume_hook = request_at_image_work_return; return; }
    assert(worker_document == race_document && !race_first->active);
    race_second = document_image_request(race_document, 0, 2, 2);
    assert(race_second && !race_document->stopped);
}

static void test_worker_stopped_request_race(void)
{
    race_document = image_document(true);
    race_first = document_image_request(race_document, 0, 2, 2); assert(race_first);
    race_second = NULL; race_done_boundaries = 0;
    resume_hook = request_at_image_work_return;
    worker_run();
    assert(!resume_hook && race_done_boundaries == 2 && race_second && race_second->done);
    assert(race_first->done && race_document->stopped && !image_pending(race_document));
    document_bitmap_t first = {0}, second = {0};
    assert(document_image_take(race_first, &first) == 1);
    assert(document_image_take(race_second, &second) == 1);
    assert(first.memory != second.memory && !memcmp(first.image.data, second.image.data, first.image.data_size));
    epd_app_free(first.memory); epd_app_free(second.memory);
    document_image_cancel(race_first); document_image_cancel(race_second);
    document_close(race_document); document_service_process(); assert_empty();
    race_first = race_second = NULL; race_document = NULL;
}

static void test_image_allocation_and_read_failure(void)
{
    /* Reader, bitmap and packed-pixel allocations are mandatory. The optional
     * read window has a separate successful fallback test. */
    const int failure_points[] = {0, 2, 3};
    for (size_t i = 0; i <= sizeof(failure_points) / sizeof(failure_points[0]); ++i)
    {
        document_t *d = image_document(true);
        document_image_request_t *job = document_image_request(d, 0, 2, 2); assert(job);
        if (i < sizeof(failure_points) / sizeof(failure_points[0])) alloc_fail_after = failure_points[i];
        else { read_fault_after = 0; read_fault_errno = EIO; }
        worker_run();
        alloc_fail_after = -1; read_fault_after = -1;
        document_bitmap_t bitmap = {0};
        assert(job->done && document_image_take(job, &bitmap) == -1 && !bitmap.memory && !job->bitmap.memory);
        document_image_cancel(job); document_close(d); document_service_process(); assert_empty();
    }
    document_t *d = image_document(true);
    alloc_fail_after = 0;
    assert(!document_image_request(d, 0, 2, 2));
    alloc_fail_after = -1;
    document_close(d); document_service_process(); assert_empty();
}

static void test_save_snapshot_and_worker(void)
{
    uint8_t header[] = {1, 2, 3}, payload[] = {4, 5, 6, 7};
    const uint8_t expected[] = {1, 2, 3, 4, 5, 6, 7};
    char path[] = "/flash/index.bin";
    unsigned before = replace_calls;
    mkdir_ok = true;
    assert(document_save_async(path, header, sizeof(header), payload, sizeof(payload)));
    memset(header, 0, sizeof(header)); memset(payload, 0, sizeof(payload)); path[1] = 'X';
    assert(replace_calls == before && saves && allocations == 1);
    worker_run();
    assert(replace_calls == before + 1 && !strcmp(replaced_path, "/flash/index.bin"));
    assert(replaced_size == sizeof(expected) && !memcmp(replaced_data, expected, sizeof(expected)));
    assert_empty(); mkdir_ok = false;
}

static void test_save_card_session(void)
{
    mkdir_ok = true;
    for (unsigned inside_lock = 0; inside_lock < 2; ++inside_lock)
    {
        unsigned before = replace_calls, directories = mkdir_calls;
        assert(document_save_async("/sdcard/index.bin", NULL, 0, "old", 3));
        if (inside_lock) remove_at_lock = true;
        else ++generation;
        worker_run();
        assert(replace_calls == before && mkdir_calls == directories);
        hotplug = false;
        assert_empty();
    }
    /* A card change does not invalidate an internal-flash destination. */
    unsigned before = replace_calls;
    assert(document_save_async("/flash/index.bin", NULL, 0, "flash", 5));
    ++generation; worker_run();
    assert(replace_calls == before + 1 && replaced_size == 5 && !memcmp(replaced_data, "flash", 5));
    assert_empty(); mkdir_ok = false;
}

static void test_save_failures_and_bounds(void)
{
    unsigned before = replace_calls;
    uint8_t byte = 0;
    char long_path[STORAGE_PATH_MAX + 1]; memset(long_path, 'x', sizeof(long_path)); long_path[STORAGE_PATH_MAX] = 0;
    assert(!document_save_async(NULL, NULL, 0, &byte, 1));
    assert(!document_save_async(long_path, NULL, 0, &byte, 1));
    assert(!document_save_async("/flash/x", NULL, 1, &byte, 1));
    assert(!document_save_async("/flash/x", &byte, 1, NULL, 1));
    assert(!document_save_async("/flash/x", &byte, SIZE_MAX, &byte, 1));
    assert(!document_save_async("/flash/x", &byte, 1, &byte, SIZE_MAX));
    assert(!document_save_async("/flash/x", NULL, 0, &byte, SIZE_MAX - sizeof(document_save_t) + 1));
    alloc_fail_after = 0;
    assert(!document_save_async("/flash/x", NULL, 0, &byte, 1));
    alloc_fail_after = -1;
    assert_empty();
    assert(document_save_async("/flash/x", NULL, 0, &byte, 1));
    mkdir_ok = false; worker_run(); assert(replace_calls == before); assert_empty();
    assert(document_save_async("/flash/x", NULL, 0, &byte, 1));
    mkdir_ok = true; replace_fail = true; worker_run();
    assert(replace_calls == before + 1); assert_empty();
    replace_fail = false;
    assert(document_save_async("/flash/x", NULL, 0, &byte, 1));
    struct epd_service stopped = {true};
    document_worker(&stopped);
    assert(replace_calls == before + 1); assert_empty(); mkdir_ok = false;
}

static document_t *index_document(void)
{
    memset(&cache_template, 0, sizeof(cache_template));
    strcpy(cache_template.binary, "/tmp/books_index_bin_XXXXXX");
    int fd = mkstemp(cache_template.binary); assert(fd >= 0);
    document_record_t record = {DOCUMENT_TEXT, 3, 0, 0};
    assert(write(fd, &record, sizeof(record)) == (ssize_t)sizeof(record));
    assert(write(fd, "abc", 3) == 3 && close(fd) == 0);
    strcpy(cache_template.index, "/tmp/books_index_idx_XXXXXX");
    fd = mkstemp(cache_template.index); assert(fd >= 0 && close(fd) == 0);
    cache_template.identity = 0x13579bdf; cache_template.end = sizeof(record) + 3;
    unsigned calls = converter_calls, touches = cache_touches;
    cache_hit = true;
    document_t *d = open_mobi("/flash/cached.mobi");
    cache_hit = false;
    assert(d->persistent && d->claimed && d->stopped && d->status.complete && !d->library && !modules);
    assert(d->status.end == cache_template.end && !strcmp(d->status.title, "Cached title"));
    assert(cache_touches == touches && d->cache_touch);
    worker_run(); assert(converter_calls == calls && cache_touches == touches + 1 && !d->cache_touch);
    mkdir_ok = replace_to_file = true;
    return d;
}

static void index_close(document_t *d)
{
    document_close(d); document_service_process(); assert_empty();
    assert(access(cache_template.binary, F_OK) == 0);
    assert(unlink(cache_template.binary) == 0 && unlink(cache_template.index) == 0);
    mkdir_ok = replace_to_file = false;
}

static void index_bytes(const void *data, size_t size)
{
    int fd = open(cache_template.index, O_WRONLY | O_TRUNC);
    assert(fd >= 0 && write(fd, data, size) == (ssize_t)size && close(fd) == 0);
}

static void index_rejected(document_t *d, uint32_t layout)
{
    void *data = (void *)&cache_template;
    size_t size = 123;
    unsigned owned = allocations;
    assert(!document_index_load(d, layout, &data, &size));
    assert(!data && size == 0 && allocations == owned);
}

static void limit_index_payload_reads(void) { read_limit = 7; }

static void test_index_roundtrip_and_short_reads(void)
{
    document_t *d = index_document();
    uint8_t payload[DOCUMENT_IO_WINDOW + 37], expected[sizeof(payload)];
    for (size_t i = 0; i < sizeof(payload); ++i) payload[i] = (uint8_t)(i * 13);
    memcpy(expected, payload, sizeof(payload));
    unsigned before = replace_calls;
    assert(document_index_save(d, 73, payload, sizeof(payload)) && replace_calls == before);
    memset(payload, 0, sizeof(payload));
    worker_run(); assert(replace_calls == before + 1);
    document_index_header_t header;
    memcpy(&header, replaced_data, sizeof(header));
    assert(header.magic == DOCUMENT_INDEX_MAGIC && header.version == 1);
    assert(header.content == cache_template.identity && header.layout == 73 && header.end == d->status.end);
    assert(header.bytes == sizeof(expected) && replaced_size == sizeof(header) + sizeof(expected));
    assert(!memcmp(replaced_data + sizeof(header), expected, sizeof(expected)));
    void *loaded = NULL; size_t length = 0;
    assert(document_index_load(d, 73, &loaded, &length));
    assert(length == sizeof(expected) && !memcmp(loaded, expected, length)); epd_app_free(loaded);
    unsigned reads = io_reads;
    read_hook = limit_index_payload_reads;
    assert(document_index_load(d, 73, &loaded, &length));
    assert(!read_hook && io_reads - reads == 1 + (sizeof(expected) + 6) / 7);
    assert(length == sizeof(expected) && !memcmp(loaded, expected, length)); epd_app_free(loaded);
    read_limit = 0;
    index_close(d);
}

static void test_index_corruption(void)
{
    document_t *d = index_document();
    const uint8_t payload[] = {7, 5, 3, 1};
    assert(document_index_save(d, 41, payload, sizeof(payload))); worker_run();
    uint8_t valid[sizeof(document_index_header_t) + sizeof(payload)];
    assert(replaced_size == sizeof(valid)); memcpy(valid, replaced_data, sizeof(valid));
    for (unsigned field = 0; field < 10; ++field)
    {
        uint8_t corrupt[sizeof(valid)]; memcpy(corrupt, valid, sizeof(corrupt));
        document_index_header_t header; memcpy(&header, valid, sizeof(header));
        switch (field)
        {
        case 0: header.magic ^= 1; break;
        case 1: ++header.version; break;
        case 2: ++header.content; break;
        case 3: ++header.layout; break;
        case 4: ++header.end; break;
        case 5: header.bytes = 0; break;
        case 6: header.bytes = UINT32_MAX; break;
        case 7: header.bytes = INT32_MAX - sizeof(header) + 1; break;
        case 8: header.checksum ^= 1; break;
        case 9: corrupt[sizeof(corrupt) - 1] ^= 1; break;
        }
        memcpy(corrupt, &header, sizeof(header));
        index_bytes(corrupt, sizeof(corrupt)); index_rejected(d, 41);
    }
    const size_t truncated[] = {0, sizeof(document_index_header_t) - 1,
                                sizeof(document_index_header_t), sizeof(valid) - 1};
    for (size_t i = 0; i < sizeof(truncated) / sizeof(truncated[0]); ++i)
    { index_bytes(valid, truncated[i]); index_rejected(d, 41); }
    uint8_t trailing[sizeof(valid) + 1]; memcpy(trailing, valid, sizeof(valid)); trailing[sizeof(valid)] = 0;
    index_bytes(trailing, sizeof(trailing)); index_rejected(d, 41);
    index_bytes(valid, sizeof(valid));
    index_rejected(d, 42);
    void *loaded = NULL; size_t length = 0;
    assert(document_index_load(d, 41, &loaded, &length) && length == sizeof(payload));
    epd_app_free(loaded); index_close(d);
}

static void test_index_io_and_allocation_failure(void)
{
    document_t *d = index_document();
    uint8_t payload[31]; memset(payload, 0xa5, sizeof(payload));
    assert(document_index_save(d, 9, payload, sizeof(payload))); worker_run();
    read_limit = sizeof(document_index_header_t) - 1;
    index_rejected(d, 9); read_limit = 0;
    for (unsigned error = 0; error < 2; ++error)
    {
        read_hook = limit_index_payload_reads;
        read_fault_after = 2; read_fault_errno = error ? EIO : 0;
        index_rejected(d, 9);
        assert(!read_hook);
        read_fault_after = -1; read_limit = 0;
    }
    alloc_fail_size = sizeof(payload);
    unsigned failures = alloc_failures;
    index_rejected(d, 9); assert(alloc_failures == failures + 1);
    alloc_fail_size = 0;
    recover_fail = true;
    unsigned opens = io_opens;
    index_rejected(d, 9); assert(io_opens == opens); recover_fail = false;
    unsigned before = replace_calls;
    alloc_fail_after = 0;
    assert(!document_index_save(d, 9, payload, sizeof(payload)) && !saves);
    alloc_fail_after = -1;
    assert(!document_index_save(d, 9, payload, 0));
    assert(!document_index_save(d, 9, NULL, sizeof(payload)));
    assert(!document_index_save(d, 9, payload, SIZE_MAX));
    assert(!document_index_save(d, 9, payload, INT32_MAX - sizeof(document_index_header_t) + 1));
    d->cached->identity = 0; assert(!document_index_save(d, 9, payload, sizeof(payload)));
    d->cached->identity = cache_template.identity;
    d->status.complete = false; assert(!document_index_save(d, 9, payload, sizeof(payload))); index_rejected(d, 9);
    d->status.complete = true;
    d->status.result = BOOK_IO_ERROR; assert(!document_index_save(d, 9, payload, sizeof(payload))); index_rejected(d, 9);
    d->status.result = BOOK_OK;
    d->persistent = false; assert(!document_index_save(d, 9, payload, sizeof(payload))); index_rejected(d, 9);
    d->persistent = true;
    assert(!saves && replace_calls == before);
    void *loaded = NULL; size_t length = 0;
    assert(document_index_load(d, 9, &loaded, &length)); epd_app_free(loaded);
    index_close(d);
}

static void test_index_card_change_under_lock(void)
{
    document_t *d = index_document();
    const uint8_t data[] = {1, 2, 3};
    assert(document_index_save(d, 1, data, sizeof(data))); worker_run();
    strcpy(d->file.path, "/sdcard/cached.mobi");
    unsigned opens = io_opens, recovered = recover_calls;
    remove_at_lock = true;
    index_rejected(d, 1);
    assert(io_opens == opens && recover_calls == recovered);
    hotplug = false;
    index_rejected(d, 1);
    assert(!document_index_save(d, 1, data, sizeof(data)) && !saves);
    index_close(d);
}

static void test_cache_invalidation_worker_only(void)
{
    for (unsigned fault = 0; fault < 5; ++fault)
    {
        document_t *d = index_document();
        unsigned invalidations = cache_invalidations, touches = cache_touches;
        if (fault < 2)
        {
            int fd = open(d->cache, O_WRONLY); assert(fd >= 0);
            document_record_t bad = {fault ? DOCUMENT_TEXT : 0, fault ? UINT32_MAX : 3, 0, 0};
            assert(write(fd, &bad, sizeof(bad)) == (ssize_t)sizeof(bad) && close(fd) == 0);
        }
        else if (fault == 4)
        {
            char payload[DOCUMENT_TEXT_LIMIT + 1]; memset(payload, 'x', sizeof(payload));
            document_record_t bad = {DOCUMENT_TEXT, sizeof(payload), 0, 0};
            int fd = open(d->cache, O_WRONLY); assert(fd >= 0);
            assert(write(fd, &bad, sizeof(bad)) == (ssize_t)sizeof(bad));
            assert(write(fd, payload, sizeof(payload)) == (ssize_t)sizeof(payload));
            struct stat st;
            assert(fstat(fd, &st) == 0 && st.st_size == (off_t)(sizeof(bad) + sizeof(payload)));
            assert(fsync(fd) == 0 && close(fd) == 0);
            d->end = d->pending_end = d->status.end = sizeof(bad) + sizeof(payload);
        }
        else { read_fault_after = 0; read_fault_errno = fault == 2 ? 0 : EIO; }
        document_record_t record;
        assert(!document_record(d, 0, &record));
        read_fault_after = -1;
        assert(d->cache_bad && !d->stopped && d->status.result == BOOK_IO_ERROR);
        assert(cache_invalidations == invalidations && cache_touches == touches);
        document_service_process();
        assert(cache_invalidations == invalidations && documents == d);
        worker_run();
        assert(cache_invalidations == invalidations + 1 && cache_touches == touches);
        assert(!d->cache_bad && d->stopped);
        index_close(d);
    }
}

static void test_cache_invalidation_survives_close(void)
{
    document_t *d = index_document();
    unsigned invalidations = cache_invalidations;
    read_fault_after = 0; read_fault_errno = EIO;
    document_record_t record;
    assert(!document_record(d, 0, &record));
    read_fault_after = -1;
    document_close(d); document_service_process();
    assert(documents == d && d->cache_bad && cache_invalidations == invalidations);
    worker_run(); assert(cache_invalidations == invalidations + 1); assert_empty();
    assert(access(cache_template.binary, F_OK) == 0);
    assert(unlink(cache_template.binary) == 0 && unlink(cache_template.index) == 0);
    mkdir_ok = replace_to_file = false;
}

static void test_worker_exit_invalidates_bad_cache(void)
{
    document_t *d = index_document();
    unsigned invalidations = cache_invalidations;
    read_fault_after = 0; read_fault_errno = EIO;
    document_record_t record;
    assert(!document_record(d, 0, &record));
    read_fault_after = -1;
    assert(d->cache_bad && !d->stopped && cache_invalidations == invalidations);
    struct epd_service stopped = {true};
    document_worker(&stopped);
    assert(cache_invalidations == invalidations + 1 && !d->cache_bad && d->stopped);
    assert(d->cancelled && worker_stopping && !worker_service && !worker_document);
    index_close(d);
}

static struct epd_service exiting_service;
static bool allocation_exit_seen;
static void exit_worker_during_save_allocation(size_t size)
{
    assert(size == sizeof(document_save_t) + 4 && saves && !worker_stopping);
    assert(worker_service == &exiting_service && allocations == 2);
    exiting_service.stop = true;
    document_worker(&exiting_service);
    assert(worker_stopping && !worker_service && !saves && allocations == 1);
    allocation_exit_seen = true;
}

static void test_save_enqueue_during_worker_exit(void)
{
    unsigned writes = replace_calls;
    mkdir_ok = true;
    exiting_service.stop = false;
    worker_service = &exiting_service;
    assert(document_save_async("/flash/queued.idx", NULL, 0, "old", 3));
    allocation_exit_seen = false; allocation_hook = exit_worker_during_save_allocation;
    assert(!document_save_async("/flash/late.idx", NULL, 0, "late", 4));
    assert(allocation_exit_seen && !allocation_hook && worker_stopping);
    assert(replace_calls == writes); assert_empty();
    assert(!document_save_async("/flash/stopped.idx", NULL, 0, "x", 1));
    assert_empty();
    worker_run();
    assert(document_save_async("/flash/shutdown1.idx", NULL, 0, "one", 3));
    assert(document_save_async("/flash/shutdown2.idx", NULL, 0, "two", 3));
    assert(allocations == 2 && saves && saves->next);
    document_service_shutdown();
    assert(worker_stopping && replace_calls == writes); assert_empty();
    assert(!document_save_async("/flash/shutdown3.idx", NULL, 0, "x", 1));
    assert_empty(); mkdir_ok = false;
}

static void set_cached_image(document_t *d, unsigned fault)
{
    document_record_t record = {DOCUMENT_IMAGE, sizeof(document_image_t) + 2, 0, 0};
    document_image_t dimensions = {2, 2};
    uint8_t payload[sizeof(dimensions) + 3] = {0};
    switch (fault)
    {
    case 1: record.kind = DOCUMENT_TEXT; break;
    case 2: record.length = sizeof(dimensions) - 1; break;
    case 3: dimensions.width = 0; break;
    case 4: dimensions.height = 0; break;
    case 5: dimensions.width = UINT32_MAX; break;
    case 6: dimensions.height = UINT32_MAX; break;
    case 7: ++record.length; break;
    default: break;
    }
    memcpy(payload, &dimensions, sizeof(dimensions));
    payload[sizeof(dimensions)] = 0x05; payload[sizeof(dimensions) + 1] = 0xaf;
    int fd = open(d->cache, O_WRONLY | O_TRUNC); assert(fd >= 0);
    assert(write(fd, &record, sizeof(record)) == (ssize_t)sizeof(record));
    assert(write(fd, payload, record.length) == (ssize_t)record.length && close(fd) == 0);
    d->end = d->pending_end = d->status.end = sizeof(record) + record.length;
}

static void cancel_bad_image_before_worker_exit(void)
{
    document_t *d = stopped_bad_document;
    assert(d && documents == d && cache_invalidations == stopped_bad_invalidations);
    if (stopped_bad_stage == 0 && d->cache_bad)
    {
        assert(worker_document == d && stopped_bad_job->active && !d->stopped);
        document_close(d);
        stopped_bad_service.stop = true;
        document_service_process();
        assert(documents == d && image_requests == stopped_bad_job && d->cache_bad);
        stopped_bad_stage = 1;
    }
    else if (stopped_bad_stage == 1 && worker_stopping)
    {
        /* The completed image task and UI cancellation must produce this
         * state before the shutdown document scan, without editing flags. */
        assert(d->stopped && d->cache_bad && d->cancelled && d->released);
        assert(!worker_document && stopped_bad_job->done && !stopped_bad_job->active);
        stopped_bad_stage = 2;
        document_service_process();
        stopped_bad_job = NULL;
        assert(!image_requests && documents == d && d->stopped && d->cache_bad);
        return;
    }
    resume_hook = cancel_bad_image_before_worker_exit;
}

static void test_worker_exit_invalidates_stopped_bad_cache(void)
{
    document_t *d = index_document(); set_cached_image(d, 3);
    stopped_bad_document = d;
    stopped_bad_job = document_image_request(d, 0, 2, 2); assert(stopped_bad_job);
    stopped_bad_service.stop = false;
    stopped_bad_stage = 0; stopped_bad_invalidations = cache_invalidations;
    resume_hook = cancel_bad_image_before_worker_exit;
    document_worker(&stopped_bad_service);
    assert(!resume_hook && stopped_bad_stage == 3);
    assert(cache_invalidations == stopped_bad_invalidations + 1);
    assert(documents == d && d->stopped && !d->cache_bad && d->released);
    assert(worker_stopping && !worker_service && !worker_document && !image_requests);
    index_close(d);
    stopped_bad_document = NULL;
}

static void test_reader_image_info_validation(void)
{
    for (unsigned fault = 0; fault <= 7; ++fault)
    {
        document_t *d = index_document(); set_cached_image(d, fault);
        document_reader_t *reader = document_reader_create(d); assert(reader);
        document_image_t image = {0};
        unsigned invalidations = cache_invalidations, owned = allocations;
        if (!fault)
        {
            unsigned reads = io_reads, opens = io_opens;
            assert(!document_reader_image_info(NULL, 0, &image));
            assert(!document_reader_image_info(reader, 0, NULL));
            assert(!document_reader_image_info(reader, UINT32_MAX, &image));
            assert(io_reads == reads && io_opens == opens && !d->cache_bad);
            alloc_fail_after = 0;
            assert(document_reader_image_info(reader, 0, &image));
            alloc_fail_after = -1;
            assert(image.width == 2 && image.height == 2 && allocations == owned);
            assert(!d->cache_bad && d->status.result == BOOK_OK);
        }
        else
        {
            assert(!document_reader_image_info(reader, 0, &image));
            assert(d->cache_bad && !d->stopped && d->status.result == BOOK_IO_ERROR);
            assert(allocations == owned && cache_invalidations == invalidations);
        }
        document_reader_destroy(reader);
        worker_run();
        assert(cache_invalidations == invalidations + (fault ? 1u : 0u));
        index_close(d);
    }
}

static void test_image_corruption_vs_allocation_failure(void)
{
    for (unsigned fault = 1; fault <= 7; ++fault)
    {
        document_t *d = index_document(); set_cached_image(d, fault);
        unsigned invalidations = cache_invalidations;
        document_bitmap_t bitmap = {0};
        assert(!document_image_load(d, 0, 2, 2, &bitmap) && !bitmap.memory);
        assert(d->cache_bad && d->status.result == BOOK_IO_ERROR && !d->stopped);
        assert(cache_invalidations == invalidations);
        worker_run(); assert(cache_invalidations == invalidations + 1 && !d->cache_bad);
        index_close(d);
    }
    const int failures[] = {0, 2, 3};
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i)
    {
        document_t *d = index_document(); set_cached_image(d, 0);
        unsigned invalidations = cache_invalidations, owned = allocations;
        document_bitmap_t bitmap = {0};
        alloc_fail_after = failures[i];
        assert(!document_image_load(d, 0, 2, 2, &bitmap) && !bitmap.memory);
        alloc_fail_after = -1;
        assert(allocations == owned && !d->cache_bad && d->status.result == BOOK_OK);
        worker_run(); assert(cache_invalidations == invalidations);
        assert(document_image_load(d, 0, 2, 2, &bitmap)); epd_app_free(bitmap.memory);
        index_close(d);
    }
    document_t *d = index_document(); set_cached_image(d, 0);
    document_bitmap_t bitmap = {0};
    unsigned invalidations = cache_invalidations;
    alloc_fail_size = DOCUMENT_IO_WINDOW;
    assert(document_image_load(d, 0, 2, 2, &bitmap));
    alloc_fail_size = 0; epd_app_free(bitmap.memory); memset(&bitmap, 0, sizeof(bitmap));
    assert(!document_image_load(d, 0, UINT_MAX, 2, &bitmap) && !bitmap.memory);
    assert(!d->cache_bad && d->status.result == BOOK_OK && cache_invalidations == invalidations);
    index_close(d);
}

static void test_integer_and_record_bounds(void)
{
    document_t d;
    fixture_open(&d, false);
    uint8_t byte = 0;
    unsigned writes = io_writes;
    d.end = INT32_MAX;
    assert(cache_room(&d, 0) == BOOK_OK && cache_room(&d, 1) == BOOK_UNSUPPORTED);
    assert(write_all(&d, &byte, 1) == BOOK_UNSUPPORTED);
    d.end = UINT32_MAX; assert(cache_room(&d, 0) == BOOK_UNSUPPORTED);
    d.end = 0;
    assert(record_write(&d, DOCUMENT_TEXT, 0, &byte, SIZE_MAX) == BOOK_UNSUPPORTED);
    assert(record_write(&d, DOCUMENT_TEXT, 0, &byte, INT32_MAX - sizeof(document_record_t) + 1) == BOOK_UNSUPPORTED);
    d.end = INT32_MAX - sizeof(document_record_t);
    assert(record_write(&d, DOCUMENT_TEXT, 0, &byte, 1) == BOOK_UNSUPPORTED && io_writes == writes);
    d.end = 0;
    document_record_t malformed[] = {{0, 0, 0, 0}, {DOCUMENT_IMAGE + 1, 0, 0, 0},
                                     {DOCUMENT_TEXT, UINT32_MAX, 0, 0}, {DOCUMENT_TEXT, 0, 0, 0}};
    assert(write_all(&d, malformed, sizeof(malformed)) == BOOK_OK);
    assert(record_complete(&d) == BOOK_OK && cache_publish(&d, true) == BOOK_OK);
    document_reader_t *r = document_reader_create(&d); assert(r);
    document_record_t record;
    for (unsigned i = 0; i < 3; ++i) assert(!document_reader_record(r, i * sizeof(record), &record));
    assert(document_reader_record(r, 3 * sizeof(record), &record) && record.length == 0);
    unsigned reads = io_reads, opens = io_opens, seeks = io_seeks;
    assert(!document_reader_read(r, 0, &byte, SIZE_MAX));
    assert(!document_reader_read(r, UINT32_MAX, &byte, 1));
    assert(!document_reader_read(r, d.status.end, &byte, 1));
    assert(!document_reader_read(r, d.status.end + 1, NULL, 0));
    assert(document_reader_read(r, d.status.end, NULL, 0));
    assert(!document_reader_read(r, 0, NULL, 1));
    assert(!document_reader_record(r, UINT32_MAX - sizeof(record) + 1, &record));
    assert(io_reads == reads && io_opens == opens && io_seeks == seeks);
    fixture_close(&d);
    document_bitmap_t bitmap = {0};
    assert(!bitmap_allocate(&bitmap, 0, 1));
    assert(!bitmap_allocate(&bitmap, UINT_MAX, 1));
    assert(!bitmap_allocate(&bitmap, (UINT16_MAX - 3u) / 2u + 1, 1));
    assert(!bitmap_allocate(&bitmap, 1, UINT16_MAX + 1u));
    assert(!bitmap.memory);
}

typedef struct { const char *name; void (*run)(void); } test_case_t;
#define TEST(name) {#name, name}
static const test_case_t cases[] = {
    TEST(test_superseded_requests), TEST(test_worker_owned_release),
    TEST(test_hotplug_cancels_source), TEST(test_cache_hotplug_inside_lock),
    TEST(test_publish_after_sync), TEST(test_record_publication_race), TEST(test_cache_capacity),
    TEST(test_small_write_bytes), TEST(test_incomplete_and_unsynced_visibility),
    TEST(test_short_io), TEST(test_partial_write_failure), TEST(test_partial_read_failure),
    TEST(test_independent_readers), TEST(test_publication_preserves_old_readers),
    TEST(test_optional_window_failure), TEST(test_image_pending_worker_take),
    TEST(test_image_cancel_pending_and_ready), TEST(test_image_inflight_lifetime),
    TEST(test_worker_stopped_request_race),
    TEST(test_image_allocation_and_read_failure), TEST(test_save_snapshot_and_worker),
    TEST(test_save_card_session), TEST(test_save_failures_and_bounds),
    TEST(test_index_roundtrip_and_short_reads), TEST(test_index_corruption),
    TEST(test_index_io_and_allocation_failure), TEST(test_index_card_change_under_lock),
    TEST(test_cache_invalidation_worker_only), TEST(test_cache_invalidation_survives_close),
    TEST(test_worker_exit_invalidates_bad_cache), TEST(test_save_enqueue_during_worker_exit),
    TEST(test_worker_exit_invalidates_stopped_bad_cache), TEST(test_reader_image_info_validation),
    TEST(test_image_corruption_vs_allocation_failure),
    TEST(test_integer_and_record_bounds)
};
#undef TEST

int main(int argc, char **argv)
{
    bool list = argc == 2 && !strcmp(argv[1], "--list");
    unsigned ran = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        if (list) { puts(cases[i].name); continue; }
        if (argc > 1 && strcmp(argv[1], cases[i].name)) continue;
        printf("RUN: %s\n", cases[i].name); fflush(stdout);
        cases[i].run(); assert_empty(); ++ran;
        printf("PASS: %s (zero owned allocations)\n", cases[i].name);
    }
    if (list) return 0;
    if (!ran) { fprintf(stderr, "Unknown test: %s\n", argc > 1 ? argv[1] : ""); return 2; }
    printf("PASS: %u service tests\n", ran);
    return 0;
}
