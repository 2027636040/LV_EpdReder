#include "covers_include/reader_stub.h"
#include "../weread_covers.c"
#include "../weread_resources.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static size_t live, allocations;
static long fail_after = -1;
static unsigned irq, http_calls, decode_calls, cleans, drops, waits, saves, wakes;
static uint32_t generation = 9;
static bool unavailable, in_worker, transport_fail, decode_fail, custom_cancel;
static int http_status = 200;
static unsigned fake_width = 7, fake_height = 3;
static void (*http_hook)(void), (*decode_hook)(void), (*allocation_hook)(void);
static char saved_book[64], saved_account[64];
static unsigned char cached_bytes[256];
static size_t cached_size;
static bool cache_enabled;
static const uint8_t *http_body = (const uint8_t *)"mock-cover-body";
static size_t http_size = 15;

rt_base_t rt_hw_interrupt_disable(void) { rt_base_t previous = (rt_base_t)irq; ++irq; return previous; }
void rt_hw_interrupt_enable(rt_base_t level) { assert(irq == (unsigned)level + 1); --irq; }
void *epd_app_alloc(size_t size, int region)
{
    (void)region; assert(!irq); ++allocations;
    if (allocation_hook) { void (*hook)(void) = allocation_hook; allocation_hook = NULL; hook(); }
    if (fail_after == 0) return NULL;
    if (fail_after > 0) --fail_after;
    void *memory = malloc(size ? size : 1);
    if (memory) ++live;
    return memory;
}
void epd_app_free(void *memory) { assert(!irq); if (memory) { assert(live); --live; free(memory); } }
bool weread_unavailable(void) { return unavailable; }
bool weread_cancelled(uint32_t serial) { return unavailable || serial != generation; }
void weread_wake(void) { assert(!irq); ++wakes; }
void epd_app_clean_draw_buffer(lv_draw_buf_t *buffer)
{ assert(!irq && !in_worker && buffer->data && buffer->data_size); assert(!((uintptr_t)buffer->data & 31)); ++cleans; }
void lv_draw_wait_for_finish(void) { assert(!in_worker); ++waits; }
void lv_image_cache_drop(const void *source) { assert(!in_worker && source); ++drops; }
void lv_image_header_cache_drop(const void *source) { assert(!in_worker && source); ++drops; }
uint32_t storage_crc32(const void *data, size_t size)
{
    /* Cache key hashing is not under test; identity is checked separately. */
    const unsigned char *p = data; uint32_t value = 1;
    while (size--) value = value * 33 + *p++;
    return value;
}
xmlChar *xmlBuildURI(const xmlChar *uri, const xmlChar *base)
{ assert(uri && base); return (xmlChar *)cover_string((const char *)uri); }
void xmlFree(void *pointer) { epd_app_free(pointer); }
void *weread_store_load(storage_app_area_t area, const char *account, const char *book, const char *key, size_t *size)
{
    assert(in_worker && !irq && area == STORAGE_APP_CACHE && account && book && key);
    assert(strlen(book) < sizeof(saved_book) && strlen(account) < sizeof(saved_account));
    strcpy(saved_book, book); strcpy(saved_account, account);
    *size = 0;
    if (!cache_enabled || !cached_size) return NULL;
    void *copy = epd_app_alloc(cached_size, EPD_APP_PSRAM);
    if (copy) { memcpy(copy, cached_bytes, cached_size); *size = cached_size; }
    return copy;
}
bool weread_store_save(storage_app_area_t area, const char *account, const char *book, const char *key, const void *data, size_t size)
{
    assert(in_worker && !irq && area == STORAGE_APP_CACHE && key);
    assert(!strcmp(account, saved_account) && !strcmp(book, saved_book)); ++saves;
    if (cache_enabled && size <= sizeof(cached_bytes)) { memcpy(cached_bytes, data, size); cached_size = size; }
    return true;
}
bool epd_http_perform(const epd_http_request_t *request, epd_http_response_t *response)
{
    assert(in_worker && !irq && !response->body && !response->size && !response->status);
    assert(!strcmp(request->method, "GET") && !strcmp(request->host, "covers.invalid"));
    assert(request->path[0] == '/' && request->header_count == 1);
    assert(!strcmp(request->headers[0].name, "Referer")); /* No Cookie or auth headers. */
    assert(request->cancelled && !request->cancelled(request->user)); ++http_calls;
    if (http_hook) { void (*hook)(void) = http_hook; http_hook = NULL; hook(); }
    if (request->cancelled(request->user) || transport_fail) return false;
    response->body = epd_app_alloc(http_size + 1, EPD_APP_PSRAM);
    if (!response->body) return false;
    memcpy(response->body, http_body, http_size); response->body[http_size] = 0;
    response->size = http_size; response->status = http_status;
    return true;
}
void epd_http_response_clear(epd_http_response_t *response)
{ epd_app_free(response->body); memset(response, 0, sizeof(*response)); }

#ifndef COVERS_REAL_RASTER
int book_image_decode(book_stream_t *stream, unsigned max_width, unsigned max_height,
    bool (*cancel)(void *), void *user, uint8_t **gray, unsigned *width, unsigned *height, char *error, size_t error_size)
{
    assert(in_worker && !irq && stream->read && max_width == 56 && max_height == 72);
    (void)error; (void)error_size; ++decode_calls;
    *gray = NULL; *width = *height = 0;
    if (decode_hook) { void (*hook)(void) = decode_hook; decode_hook = NULL; hook(); }
    if (cancel(user)) return BOOK_CANCELLED;
    if (decode_fail) return BOOK_ERROR;
    *gray = epd_app_alloc(fake_width * fake_height, EPD_APP_PSRAM);
    if (!*gray) return BOOK_NO_MEMORY;
    *width = fake_width; *height = fake_height;
    for (unsigned i = 0; i < fake_width * fake_height; ++i) (*gray)[i] = (uint8_t)((i % 16) * 17);
    return BOOK_OK;
}
#endif

static wr_book_t test_books[12];
static weread_result_t snapshot;
static void reset(void)
{
    assert(!in_worker); weread_covers_shutdown(); assert(!live && !irq);
    generation = 9; unavailable = false; fail_after = -1;
    http_calls = decode_calls = cleans = drops = waits = saves = wakes = 0;
    http_hook = decode_hook = allocation_hook = NULL;
    transport_fail = decode_fail = custom_cancel = cache_enabled = false;
    cached_size = 0; http_status = 200; fake_width = 7; fake_height = 3;
    memset(test_books, 0, sizeof(test_books));
    for (unsigned i = 0; i < 12; ++i) { test_books[i].id = "book-a"; test_books[i].cover = "https://covers.invalid/a.png"; }
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.account = "account-a"; snapshot.generation = generation; snapshot.revision = 3;
    snapshot.books = test_books; snapshot.book_count = 12;
}
static bool step(void) { in_worker = true; bool attempted = weread_covers_step(); in_worker = false; return attempted; }
static void ui_cancel(void) { bool worker = in_worker; in_worker = false; weread_covers_cancel(); in_worker = worker; }
static uint32_t replacement;
static void ui_replace(void)
{
    bool worker = in_worker; in_worker = false;
    test_books[7].id = "book-b"; test_books[7].cover = "https://covers.invalid/b.png";
    replacement = weread_covers_request(&snapshot, 7, 1); assert(replacement);
    in_worker = worker;
}
static void stop_service(void) { unavailable = true; }
static void new_generation(void) { ++generation; }
static bool custom_stop(void *user) { return *(bool *)user; }
static void stop_custom(void) { custom_cancel = true; }

#ifndef COVERS_REAL_RASTER
static void test_visible(void)
{
    reset(); uint32_t request = weread_covers_request(&snapshot, 2, SIZE_MAX); assert(request);
    assert(!http_calls && !decode_calls); snapshot.account = "changed"; test_books[2].cover = "changed";
    for (size_t i = 0; i < 8; ++i)
    {
        assert(step() && !step());
        weread_cover_t *cover = weread_covers_take(); assert(cover);
        assert(cover->request == request && cover->generation == 9 && cover->revision == 3 && cover->index == i + 2);
        assert(!strcmp(cover->account, "account-a") && !strcmp(cover->book, "book-a"));
        assert(!strcmp(cover->url, "https://covers.invalid/a.png") && !strcmp(saved_book, cover->book));
        assert(cover->image.header.w == 7 && cover->image.header.h == 3 && cover->image.header.stride == 16);
        assert(cover->image.header.cf == LV_COLOR_FORMAT_RGB565 && cover->image.data_size == 48);
        assert(!((uintptr_t)cover->image.data & 31));
        const uint16_t *pixels = (const uint16_t *)cover->image.data;
        assert(pixels[0] == 0 && pixels[1] == 0x1082 && pixels[7] == 0xffff);
        assert(!weread_covers_take()); weread_covers_free(cover);
    }
    assert(!step() && http_calls == 8 && decode_calls == 8 && cleans == 8 && drops == 16 && waits == 8 && !live);
    reset(); test_books[1].cover = "";
    assert(weread_covers_request(&snapshot, 0, 3));
    assert(step()); weread_covers_free(weread_covers_take());
    assert(step()); weread_cover_t *cover = weread_covers_take(); assert(cover && cover->index == 2);
    weread_covers_free(cover); assert(!step() && http_calls == 2 && !live);
    puts("covers: visible cap, copied identity/URL, one-result backpressure, alignment and UI cleanup passed");
}
static void test_cancellation(void)
{
    reset(); assert(weread_covers_request(&snapshot, 0, 8)); weread_covers_cancel(); assert(!step() && !live);
    reset(); assert(weread_covers_request(&snapshot, 0, 8)); assert(step()); weread_covers_cancel(); assert(!live);
    reset(); assert(weread_covers_request(&snapshot, 0, 8)); http_hook = ui_cancel;
    assert(step() && !weread_covers_take() && !step() && !live && !saves && !decode_calls);
    reset(); assert(weread_covers_request(&snapshot, 0, 8)); decode_hook = ui_cancel;
    assert(step() && !weread_covers_take() && !step() && !live);
    reset(); assert(weread_covers_request(&snapshot, 0, 8)); http_hook = ui_replace;
    assert(step() && !weread_covers_take()); assert(step());
    weread_cover_t *cover = weread_covers_take(); assert(cover && cover->request == replacement && cover->index == 7);
    assert(!strcmp(cover->book, "book-b") && !strcmp(saved_book, "book-b"));
    weread_covers_free(cover); assert(!live);
    reset(); allocation_hook = ui_cancel; assert(!weread_covers_request(&snapshot, 0, 8)); assert(!live && !step());
    reset(); assert(weread_covers_request(&snapshot, 0, 8)); http_hook = new_generation;
    assert(step() && !weread_covers_take() && !step() && !live);
    reset(); assert(weread_covers_request(&snapshot, 0, 8)); assert(step()); ++generation;
    assert(!weread_covers_take()); weread_covers_shutdown(); assert(!live);
    reset(); assert(weread_covers_request(&snapshot, 0, 8)); assert(step()); cover = weread_covers_take();
    weread_covers_shutdown(); assert(live && cover); weread_covers_free(cover); assert(!live);
    puts("covers: cancellation during allocation/HTTP/decode, same-generation replacement, stale output and shutdown passed");
}
static void test_failures(void)
{
    for (unsigned kind = 0; kind < 5; ++kind)
    {
        reset(); assert(weread_covers_request(&snapshot, 0, 1));
        if (kind == 0) transport_fail = true;
        if (kind == 1) http_status = 403;
        if (kind == 2) decode_fail = true;
        if (kind == 3) fake_width = 57;
        if (kind == 4) http_hook = stop_service;
        assert(step() && !weread_covers_take() && !step() && !live);
    }
    for (long fail = 0; fail < 55; ++fail)
    {
        reset(); fail_after = fail; (void)weread_covers_request(&snapshot, 0, 8);
        while (step()) weread_covers_free(weread_covers_take());
        weread_covers_cancel(); fail_after = -1; assert(!live && !irq);
    }
    reset(); assert(!weread_covers_request(NULL, 0, 1));
    assert(!weread_covers_request(&snapshot, SIZE_MAX, SIZE_MAX));
    assert(!weread_covers_request(&snapshot, 0, 0));
    snapshot.account = NULL; assert(!weread_covers_request(&snapshot, 0, 1)); assert(!live);
    puts("covers: transport/status/decode failures, malformed dimensions, invalid ranges and 55 OOM positions passed");
}
#endif
static void test_resources(void)
{
    reset(); snapshot.book = "book-a"; book_stream_t stream;
    in_worker = true; ++generation;
    assert(weread_resource_open(&snapshot, NULL, "https://covers.invalid/a.png", &stream) == BOOK_CANCELLED);
    assert(weread_resource_open_ex(&snapshot, NULL, "https://covers.invalid/a.png", &stream, custom_stop, &custom_cancel) == BOOK_OK);
    assert(stream.size == http_size); uint8_t byte;
    assert(stream.read(stream.user, &byte, 1) == 1 && byte == http_body[0]);
    assert(stream.seek(stream.user, http_size + 1) == BOOK_IO_ERROR);
    stream.close(stream.user);
    http_hook = stop_custom;
    assert(weread_resource_open_ex(&snapshot, NULL, "https://covers.invalid/a.png", &stream, custom_stop, &custom_cancel) == BOOK_CANCELLED);
    assert(!stream.user && !stream.close);
    custom_cancel = false; unavailable = true;
    assert(weread_resource_open_ex(&snapshot, NULL, "https://covers.invalid/a.png", &stream, custom_stop, &custom_cancel) == BOOK_CANCELLED);
    unavailable = false; snapshot.generation = generation; cache_enabled = true;
    assert(weread_resource_open(&snapshot, NULL, "https://covers.invalid/a.png", &stream) == BOOK_OK); stream.close(stream.user);
    unsigned requests = http_calls;
    assert(weread_resource_open(&snapshot, NULL, "https://covers.invalid/a.png", &stream) == BOOK_OK); stream.close(stream.user);
    assert(http_calls == requests && !live);
    assert(weread_resource_open(&snapshot, NULL, "http://covers.invalid/a.png", &stream) == BOOK_UNSUPPORTED);
    in_worker = false;
    puts("resources: generation default/custom override, service fallback, HTTP cancellation, cache and no auth headers passed");
}

#ifndef COVERS_REAL_RASTER
int main(void)
{
    test_visible(); test_cancellation(); test_failures(); test_resources();
    weread_covers_shutdown(); assert(!live && !irq);
    printf("PASS: covers/resources; %zu checked allocations; no live allocations; HTTP/storage/decoder mocked.\n", allocations);
    return 0;
}
#endif
