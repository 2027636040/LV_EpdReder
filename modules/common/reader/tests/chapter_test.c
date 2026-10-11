/* Real chapter/stream/XML/document/cache pipeline with a host UI boundary. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <rthw.h>
#include "../reader_internal.h"
#include "../reader_resource.h"
#include "../../../books/document/document_cache.h"
#include "../../../books/document/tests/cache/platform.h"

struct epd_service { bool stop; };
static struct epd_service service;
static bool on_worker;
static document_t *current;
static unsigned releases, resources_opened, resources_closed, image_decodes;
static char last_error[96];
typedef struct { bool cancel, close_in_resource; int resource_result; unsigned remaining; } context_t;
static context_t context;
static reader_position_t restored;

rt_tick_t rt_tick_get(void) { static rt_tick_t tick; return ++tick; }
rt_tick_t rt_tick_from_millisecond(int milliseconds) { return milliseconds; }
rt_base_t rt_hw_interrupt_disable(void) { return 0; }
void rt_hw_interrupt_enable(rt_base_t state) { (void)state; }
bool epd_service_cancelled(epd_service_t *s) { return s->stop; }
void epd_service_wake(epd_service_t *s) { (void)s; }
uint32_t epd_service_wait(epd_service_t *s, rt_int32_t timeout)
{ (void)s; (void)timeout; abort(); }
void epd_app_clean_draw_buffer(lv_draw_buf_t *buffer) { (void)buffer; assert(!on_worker); }
bool storage_info(storage_volume_t volume, storage_info_t *info)
{ (void)volume; memset(info, 0, sizeof(*info)); return false; }
void *epd_app_library_open(const char *app, const char *name)
{ (void)app; (void)name; abort(); }
void *epd_app_library_symbol(void *library, const char *name)
{ (void)library; (void)name; abort(); }
void epd_app_library_close(void *library) { (void)library; abort(); }

bool epd_reader_attach(document_t *document, const char *title, const reader_position_t *restore,
                   reader_save_fn save, document_t *(*reopen)(void *, char *, size_t), void *user)
{
    assert(!on_worker && document && title && restore && !reopen && user == &context);
    (void)save;
    current = document; restored = *restore; return true;
}
void epd_reader_close(void) { assert(!on_worker); document_close(current); current = NULL; }
void epd_reader_collect(void) { assert(!on_worker); document_service_process(); }
void epd_reader_set_error(const char *message) { snprintf(last_error, sizeof(last_error), "%s", message); }
static bool is_cancelled(void *user) { return ((context_t *)user)->cancel; }
static void release_context(void *user) { assert(!on_worker && user == &context); ++releases; }
static int image_read(void *user, void *buffer, size_t length)
{
    context_t *c = user;
    assert(on_worker);
    if (!length || !c->remaining) return 0;
    *(char *)buffer = 'x'; --c->remaining; return 1;
}
static void image_close(void *user) { assert(on_worker && user == &context); ++resources_closed; }
static int resource_open(void *user, const char *base, const char *reference, book_stream_t *stream)
{
    context_t *c = user;
    assert(on_worker && strstr(base, "chapter") && strstr(reference, "picture.png"));
    ++resources_opened;
    if (c->close_in_resource)
    {
        unsigned before = releases;
        on_worker = false; epd_reader_close(); epd_reader_collect(); on_worker = true;
        assert(releases == before); /* Claimed session still belongs to this worker. */
    }
    if (c->resource_result) return c->resource_result;
    c->remaining = 8;
    *stream = (book_stream_t){c, 8, image_read, NULL, image_close};
    return BOOK_OK;
}
int document_image_decode(book_stream_t *stream, bool (*cancelled)(void *), void *user,
                          uint8_t **gray, unsigned *width, unsigned *height)
{
    assert(on_worker); ++image_decodes;
    if (cancelled(user)) return BOOK_CANCELLED;
    char byte;
    if (stream->read(stream->user, &byte, 1) != 1) return BOOK_IO_ERROR;
    *gray = epd_app_alloc(4, EPD_APP_PSRAM);
    if (!*gray) return BOOK_NO_MEMORY;
    memset(*gray, 170, 4); *width = *height = 2; return BOOK_OK;
}
static void drain(void)
{
    assert(!on_worker); on_worker = true;
    unsigned steps = 0;
    while (epd_reader_worker_step(&service)) assert(++steps < 100);
    on_worker = false;
}
static document_status_t status(void)
{ document_status_t s; assert(current); document_status(current, &s); return s; }
static reader_options_t options(const char *identity, reader_source_kind_t kind)
{
    return (reader_options_t){.app_id = "weread", .cache_path = "/flash/cache/weread/layout",
        .content_identity = identity, .path = "/flash/chapter.raw", .title = "Chapter title", .kind = kind,
        .restore = {{0, 0}, DOCUMENT_CONVERSION_VERSION, 123, 1, 2, 3},
        .resources = {&context, resource_open}, .cancelled = is_cancelled,
        .release_user = release_context, .user = &context};
}
static void close_current(void)
{
    unsigned before = releases;
    epd_reader_close(); drain(); epd_reader_collect();
    assert(releases == before + 1 && !test_allocations);
}
static size_t collect_text(char *text, size_t capacity, unsigned *styles, unsigned *images)
{
    document_status_t s = status();
    assert(s.complete && s.result == BOOK_OK);
    document_reader_t *reader = document_reader_create(current); assert(reader);
    size_t used = 0;
    *styles = *images = 0;
    for (uint32_t offset = 0; offset < s.end;)
    {
        document_record_t record;
        assert(document_reader_record(reader, offset, &record));
        if (record.kind == DOCUMENT_TEXT)
        {
            assert(used + record.length < capacity);
            assert(document_reader_read(reader, offset + sizeof(record), text + used, record.length));
            used += record.length; *styles |= record.style;
        }
        if (record.kind == DOCUMENT_IMAGE) ++*images;
        offset += sizeof(record) + record.length;
    }
    text[used] = 0;
    document_reader_destroy(reader);
    return used;
}
static void txt_and_identity(void)
{
    char input[10003], text[11000];
    memset(input, 'a', sizeof(input)); memcpy(input, "\xef\xbb\xbf", 3);
    memcpy(input + 4099, "\xe4\xb8\xad", 3); input[7000] = '\n';
    test_put("/flash/chapter.raw", input, sizeof(input));
    strcpy(test_source, "/flash/chapter.raw");
    test_source_opens = 0;
    reader_options_t o = options("account:book:chapter:v1", READER_SOURCE_TXT);
    assert(epd_reader_open(&o) && !status().complete && !test_source_opens);
    assert(restored.layout == 123);
    test_short_read = true; drain(); test_short_read = false;
    unsigned styles, images;
    assert(collect_text(text, sizeof(text), &styles, &images) == sizeof(input) - 3);
    assert(!memcmp(text, input + 3, sizeof(input) - 3) && !images);
    assert(test_source_opens == 1);
    uint32_t bytes = status().end;
    void *first = malloc(bytes), *second = malloc(bytes);
    assert(first && second && document_read(current, 0, first, bytes));
    close_current();
    o.content_identity = "account:book:chapter:normal-io";
    assert(epd_reader_open(&o)); drain();
    assert(status().end == bytes && document_read(current, 0, second, bytes));
    assert(!memcmp(first, second, bytes)); free(first); free(second); close_current();
    o.content_identity = "account:book:chapter:v1";
    test_remove(o.path);
    assert(epd_reader_open(&o) && status().complete); /* Cached conversion needs no source open. */
    drain(); assert(test_source_opens == 2); close_current();
    test_put(o.path, "new", 3);
    o.content_identity = "account:book:chapter:v2";
    assert(epd_reader_open(&o) && !status().complete); drain();
    assert(collect_text(text, sizeof(text), &styles, &images) == 3 && !strcmp(text, "new")); close_current();
    o.app_id = "another_app";
    assert(epd_reader_open(&o) && !status().complete); drain(); close_current();
    o.cache_path = "/flash/cache/weread/other";
    assert(epd_reader_open(&o) && !status().complete); drain(); close_current();
    puts("PASS TXT >1600 bytes, UTF-8/BOM/read boundaries, restore forwarding, cache hit, identity/app/directory isolation");
}
static void markup_and_resources(void)
{
    static const char xhtml[] = "<html><body><h1 id='top'>Heading</h1><p>A <b>bold</b> "
        "<a href='#top'>link</a></p><img src='https://example.test/picture.png'/>"
        "<img src='picture.png'/><p>After pictures</p></body></html>";
    test_put("/flash/chapter.raw", xhtml, sizeof(xhtml) - 1);
    reader_options_t o = options("xhtml:v1", READER_SOURCE_XHTML);
    char text[2048]; unsigned styles, images;
    resources_opened = resources_closed = image_decodes = 0;
    assert(epd_reader_open(&o)); drain();
    collect_text(text, sizeof(text), &styles, &images);
    assert(strstr(text, "Heading") && strstr(text, "After pictures"));
    assert(styles & BOOK_STYLE_BOLD); assert(styles & BOOK_STYLE_LINK);
    assert(images == 2 && resources_opened == 2 && resources_closed == 2 && image_decodes == 2);
    close_current();
    o.content_identity = "xhtml:offline"; context.resource_result = BOOK_IO_ERROR;
    assert(epd_reader_open(&o)); drain(); collect_text(text, sizeof(text), &styles, &images);
    assert(!images && strstr(text, "图片暂不可用") && strstr(text, "After pictures")); close_current();
    assert(epd_reader_open(&o) && !status().complete); /* Placeholder is not persisted as complete. */
    context.resource_result = 0; drain(); collect_text(text, sizeof(text), &styles, &images);
    assert(images == 2); close_current();
    o.content_identity = "xhtml:oom"; context.resource_result = BOOK_NO_MEMORY;
    assert(epd_reader_open(&o)); drain(); assert(status().result == BOOK_NO_MEMORY); close_current();
    o.content_identity = "xhtml:cancel"; context.resource_result = BOOK_CANCELLED;
    assert(epd_reader_open(&o)); drain(); assert(status().result == BOOK_CANCELLED); close_current();
    context.resource_result = 0; context.close_in_resource = true;
    o.content_identity = "xhtml:close-in-worker";
    unsigned before = releases;
    assert(epd_reader_open(&o)); drain(); assert(!current && releases == before);
    epd_reader_collect(); assert(releases == before + 1 && !test_allocations);
    context.close_in_resource = false;
    puts("PASS XHTML structure/links, remote/local resources, stream closure, offline placeholders/retry, OOM/cancel and in-flight release");
}
static const char css_chapter[] = "<html><head><link rel='stylesheet' href='style.css'/></head>"
    "<body><p class='emphasis'>Offline chapter body</p></body></html>";
static const char css_text[] = ".emphasis { font-weight: bold; }";
typedef struct { const char *text; size_t size, offset; int error; } text_stream_t;
static text_stream_t css_stream;
static int css_open_result, css_read_result;
static bool css_stream_on_error;
static unsigned css_opened, css_closed;
static int text_read(void *user, void *buffer, size_t length)
{
    text_stream_t *s = user;
    assert(on_worker);
    if (s->error) return s->error;
    if (length > s->size - s->offset) length = s->size - s->offset;
    memcpy(buffer, s->text + s->offset, length); s->offset += length;
    return (int)length;
}
static void css_close(void *user)
{ assert(on_worker && user == &css_stream); ++css_closed; }
static int css_open(void *user, const char *base, const char *reference, book_stream_t *stream)
{
    assert(on_worker && user == &context && strstr(base, "chapter") && !strcmp(reference, "style.css"));
    ++css_opened;
    memset(stream, 0, sizeof(*stream));
    if (!css_open_result || css_stream_on_error)
    {
        css_stream = (text_stream_t){css_text, sizeof(css_text) - 1, 0, css_read_result};
        *stream = (book_stream_t){&css_stream, css_stream.size, text_read, NULL, css_close};
    }
    return css_open_result;
}
static void markup_missing_css(void)
{
    test_put("/flash/chapter.raw", css_chapter, sizeof(css_chapter) - 1);
    reader_options_t o = options("xhtml:css-fallback", READER_SOURCE_XHTML);
    o.resources.open = css_open;
    char text[128]; unsigned styles, images;
    const int missing[] = {BOOK_IO_ERROR, BOOK_ERROR, BOOK_UNSUPPORTED};
    css_opened = css_closed = 0;
    css_stream_on_error = true;
    for (unsigned i = 0; i < sizeof(missing) / sizeof(missing[0]); ++i)
    {
        css_open_result = missing[i];
        assert(epd_reader_open(&o) && !status().complete); drain();
        collect_text(text, sizeof(text), &styles, &images);
        assert(!strcmp(text, "Offline chapter body") && styles == BOOK_STYLE_NORMAL && !images);
        assert(css_opened == i + 1 && css_closed == css_opened);
        close_current(); /* Failed-open streams close once; no fallback document is persisted. */
    }
    css_open_result = BOOK_OK; css_stream_on_error = false;
    assert(epd_reader_open(&o) && !status().complete); drain();
    collect_text(text, sizeof(text), &styles, &images);
    assert(!strcmp(text, "Offline chapter body") && (styles & BOOK_STYLE_BOLD) && !images);
    assert(css_opened == 4 && css_closed == 4); close_current();
    assert(epd_reader_open(&o) && status().complete); drain();
    collect_text(text, sizeof(text), &styles, &images);
    assert(styles & BOOK_STYLE_BOLD); assert(css_opened == 4 && css_closed == 4); close_current();

    /* Fatal open errors are not converted into the missing-resource marker. */
    const int fatal[] = {BOOK_NO_MEMORY, BOOK_CANCELLED, BOOK_NO_SPACE};
    o.content_identity = "xhtml:css-fatal"; css_stream_on_error = true;
    for (unsigned i = 0; i < sizeof(fatal) / sizeof(fatal[0]); ++i)
    {
        css_open_result = fatal[i];
        assert(epd_reader_open(&o) && !status().complete); drain();
        assert(status().result == fatal[i] && css_opened == css_closed); close_current();
    }
    css_open_result = BOOK_OK; css_stream_on_error = false;
    const int read_errors[] = {BOOK_IO_ERROR, BOOK_NO_MEMORY, BOOK_CANCELLED, READER_RESOURCE_MISSING};
    for (unsigned i = 0; i < sizeof(read_errors) / sizeof(read_errors[0]); ++i)
    {
        css_read_result = read_errors[i];
        assert(epd_reader_open(&o) && !status().complete); drain();
        assert(status().result == read_errors[i] && css_opened == css_closed); close_current();
    }
    css_read_result = BOOK_OK;
    puts("PASS CSS adapter: missing styles preserve body/default style, retry gains CSS/cache hit, fatal/read errors propagate, streams close once");
}
static void markup_books_css_errors(void)
{
    /* Bypass the chapter adapter: ordinary books resource errors must be unchanged. */
    const int errors[] = {BOOK_IO_ERROR, BOOK_ERROR, BOOK_UNSUPPORTED,
                          BOOK_NO_MEMORY, BOOK_CANCELLED, BOOK_NO_SPACE};
    book_allocator_t allocator = {malloc, realloc, free};
    book_sink_t sink = {0};
    book_resources_t resources = {&context, css_open};
    css_stream_on_error = false; css_read_result = BOOK_OK;
    for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i)
    {
        text_stream_t input = {css_chapter, sizeof(css_chapter) - 1, 0, BOOK_OK};
        book_stream_t stream = {.user = &input, .size = input.size, .read = text_read};
        css_open_result = errors[i];
        unsigned opened = css_opened, closed = css_closed;
        on_worker = true;
        assert(book_markup_begin(&allocator) == BOOK_OK);
        assert(book_markup_convert(&sink, &stream, "/flash/chapter.raw", &resources) == errors[i]);
        book_markup_end(); on_worker = false;
        assert(css_opened == opened + 1 && css_closed == closed && !test_allocations);
    }
    css_open_result = BOOK_OK;
    puts("PASS books markup: unadapted IO/error/unsupported/OOM/cancel/no-space errors retain original results");
}
static void failures_and_stop(void)
{
    reader_options_t o = options("pending:close", READER_SOURCE_TXT);
    unsigned before = releases;
    assert(epd_reader_open(&o)); epd_reader_close(); epd_reader_collect();
    assert(releases == before + 1 && !test_allocations);
    char long_value[512]; memset(long_value, 'a', sizeof(long_value) - 1); long_value[511] = 0;
    o.path = long_value; before = releases;
    assert(!epd_reader_open(&o) && releases == before + 1 && *last_error && !test_allocations);
    o = options(long_value, READER_SOURCE_TXT); before = releases;
    assert(!epd_reader_open(&o) && releases == before + 1 && !test_allocations);
    o = options("alloc:failure", READER_SOURCE_TXT); before = releases;
    test_fail_alloc = 0; assert(!epd_reader_open(&o)); test_fail_alloc = -1;
    assert(releases == before + 1 && !test_allocations);
    o = options("cancel:pending", READER_SOURCE_TXT);
    assert(epd_reader_open(&o)); context.cancel = true; drain();
    assert(status().result == BOOK_CANCELLED); close_current(); context.cancel = false;
    o = options("cache:replacement-card", READER_SOURCE_TXT);
    o.cache_path = "/sdcard/.epd/cache/weread/layout";
    assert(epd_reader_open(&o)); ++test_generation;
    unsigned io_before = test_io;
    drain(); assert(status().result == BOOK_CANCELLED && test_io == io_before);
    close_current();
    o = options("stop:pending", READER_SOURCE_TXT);
    assert(epd_reader_open(&o)); on_worker = true; service.stop = true;
    epd_reader_worker_shutdown(); on_worker = false;
    before = releases; epd_reader_close(); epd_reader_collect();
    assert(releases == before + 1 && !test_allocations);
    service.stop = false;
    o = options("restart:after-join", READER_SOURCE_TXT);
    assert(epd_reader_open(&o)); drain(); close_current();
    on_worker = true; epd_reader_worker_shutdown(); on_worker = false;
    puts("PASS close-before-worker, explicit length rejection, allocation failure, cancellation, shutdown/join/restart; zero owned allocations");
}
static void markup_short_reads(void)
{
    char input[10040];
    const char *prefix = "<html><body><p>";
    size_t start = strlen(prefix);
    memcpy(input, prefix, start); memset(input + start, 'z', 10000);
    const char *suffix = "</p></body></html>";
    memcpy(input + start + 10000, suffix, strlen(suffix));
    test_put("/flash/chapter.raw", input, start + 10000 + strlen(suffix));
    reader_options_t o = options("xhtml:normal-boundaries", READER_SOURCE_XHTML);
    assert(epd_reader_open(&o)); drain();
    uint32_t bytes = status().end;
    void *first = malloc(bytes), *second = malloc(bytes);
    assert(first && second && document_read(current, 0, first, bytes)); close_current();
    o.content_identity = "xhtml:short-boundaries";
    assert(epd_reader_open(&o)); test_short_read = true; drain(); test_short_read = false;
    assert(status().end == bytes && document_read(current, 0, second, bytes));
    assert(!memcmp(first, second, bytes)); free(first); free(second); close_current();
    puts("PASS XHTML normal and short reads produce identical record/cursor boundaries");
}
int main(int argc, char **argv)
{
    setbuf(stdout, NULL);
    assert(argc == 2); test_platform_init(argv[1]);
    txt_and_identity(); markup_and_resources(); markup_missing_css(); markup_books_css_errors();
    markup_short_reads(); failures_and_stop();
    return 0;
}
