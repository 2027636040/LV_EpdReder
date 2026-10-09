#define _XOPEN_SOURCE 700
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define fsync mock_fsync
#include "../document.c"
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

void *epd_app_alloc(size_t n, int region) { (void)region; void *p = malloc(n); if (p) ++allocations; return p; }
void *epd_app_realloc(void *p, size_t n, int region)
{ (void)region; if (!p) return epd_app_alloc(n, region); return realloc(p, n); }
void epd_app_free(void *p) { if (p) { assert(allocations); --allocations; free(p); } }
rt_tick_t rt_tick_get(void) { return ticks; }
rt_tick_t rt_tick_from_millisecond(int ms) { return ms; }
int mock_fsync(int fd) { assert(fd >= 0); ++syncs; return fail_sync ? -1 : 0; }
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
}
const char *storage_path_root(const char *p) { return !strncmp(p, "/sdcard", 7) ? STORAGE_SD_ROOT : STORAGE_FLASH_ROOT; }
bool storage_path_available(const char *p) { return !hotplug || strcmp(storage_path_root(p), STORAGE_SD_ROOT); }
bool storage_available(storage_volume_t v) { return v != STORAGE_SD || !hotplug; }
uint32_t storage_revision(void) { return generation; }
uint32_t storage_card_session(void) { return generation; }
bool storage_app_path(char *out, size_t size, const char *id, storage_app_area_t area, const char *relative)
{
    assert(!strcmp(id, "books") && area == STORAGE_APP_CACHE);
    int n = snprintf(out, size, "/flash/cache/books/%s", relative);
    return n > 0 && (size_t)n < size;
}
bool storage_changing(void) { return hotplug; }
void storage_lock(void)
{ if (remove_at_lock) { remove_at_lock = false; ++generation; hotplug = true; } }
void storage_unlock(void) {}
bool storage_mkdirs(const char *p) { (void)p; return false; }
bool storage_info(storage_volume_t v, storage_info_t *info) { (void)v; memset(info, 0, sizeof(*info)); return false; }
bool storage_file_signature(const char *p, uint32_t *out) { (void)p; *out = 1; return true; }
void *epd_app_library_open(const char *app, const char *name)
{ assert(!strcmp(app, "books") && !strcmp(name, "bk_mobi")); ++modules; return (void *)1; }
void *epd_app_library_symbol(void *module, const char *name)
{ assert(module && !strcmp(name, "book_converter")); return (void *)&mock_mobi; }
void epd_app_library_close(void *module) { assert(module && modules); --modules; }
void epd_app_clean_draw_buffer(lv_draw_buf_t *buf) { assert(buf->data && buf->data_size); }
bool epd_service_cancelled(epd_service_t *s) { return s->stop; }
void epd_service_wake(epd_service_t *s) { (void)s; }
uint32_t epd_service_wait(epd_service_t *s, rt_int32_t ticks)
{
    (void)ticks;
    document_service_process();
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
{ (void)p; (void)s; (void)a; (void)error; (void)size; return BOOK_UNSUPPORTED; }
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
{ assert(!documents && !worker_document && !modules && !allocations); }

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
    close(d.input); close(d.output); unlink(d.cache);
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
    if (d.input >= 0) close(d.input);
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

int main(void)
{
    test_superseded_requests(); test_worker_owned_release(); test_hotplug_cancels_source();
    test_cache_hotplug_inside_lock();
    test_publish_after_sync();
    test_record_publication_race();
    test_cache_capacity();
    puts("PASS: 100 cancellations with private library cleanup, source/cache hotplug under lock, batched publication after sync, reader reopen, publication race bounds, zero owned allocations");
    return 0;
}
