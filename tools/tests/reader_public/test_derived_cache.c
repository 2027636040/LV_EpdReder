#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif
#include "lvgl.h"
#include "epd_memory.h"

static unsigned waits, copies, frees, flushes, source_releases;
static bool fail_pixels;
static size_t available = 64u * 1024u * 1024u;
static lv_draw_buf_t *source_pixels;

void *epd_app_alloc(size_t bytes, epd_app_memory_t region)
{ (void)region; return malloc(bytes); }
void epd_app_free(void *ptr) { free(ptr); }
size_t epd_app_psram_available(void) { return available; }
static void test_draw_wait_for_finish(void)
{ assert(!test_mutex_depth); ++waits; }

#define lv_draw_wait_for_finish test_draw_wait_for_finish
#include "../../../src/ui/ui_reader_font_cache.c"
#undef lv_draw_wait_for_finish
#include "derived_api_h.h"
#define RTM_EXPORT(name)
#include "derived_api_c.h"

static void *pixel_malloc(size_t bytes, lv_color_format_t format)
{
    (void)format;
    if (fail_pixels) return NULL;
    ++copies;
    return malloc(bytes + LV_DRAW_BUF_ALIGN - 1);
}
static void pixel_free(void *ptr) { ++frees; free(ptr); }
static void pixel_flush(const lv_draw_buf_t *buffer, const lv_area_t *area)
{ (void)area; assert(buffer && buffer->data); ++flushes; }
static bool source_descriptor(const lv_font_t *font, lv_font_glyph_dsc_t *glyph,
                              uint32_t letter, uint32_t next)
{
    (void)next;
    memset(glyph, 0, sizeof(*glyph));
    glyph->resolved_font = font;
    glyph->gid.index = letter;
    glyph->box_w = 8;
    glyph->box_h = 8;
    glyph->stride = 8;
    glyph->adv_w = 8;
    glyph->format = LV_FONT_GLYPH_FORMAT_A8;
    return true;
}
static const void *source_bitmap(lv_font_glyph_dsc_t *glyph, lv_draw_buf_t *scratch)
{ (void)glyph; (void)scratch; return source_pixels; }
static void source_release(const lv_font_t *font, lv_font_glyph_dsc_t *glyph)
{ (void)font; ++source_releases; glyph->entry = NULL; }
static lv_font_t make_font(void)
{
    lv_font_t font = {0};
    font.get_glyph_dsc = source_descriptor;
    font.get_glyph_bitmap = source_bitmap;
    font.release_glyph = source_release;
    font.kerning = LV_FONT_KERNING_NONE;
    return font;
}
static void release_entry(lv_cache_entry_t **entry)
{ ui_reader_font_derived_release(*entry); *entry = NULL; }

int main(void)
{
#ifdef _MSC_VER
    _set_error_mode(_OUT_TO_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    lv_init();
    lv_draw_buf_handlers_t *handlers = lv_draw_buf_get_font_handlers();
    handlers->buf_malloc_cb = pixel_malloc;
    handlers->buf_free_cb = pixel_free;
    handlers->flush_cache_cb = pixel_flush;
    source_pixels = lv_draw_buf_create(8, 8, LV_COLOR_FORMAT_A8, 8);
    assert(source_pixels);
    memset(source_pixels->data, 0x3a, 64);
    lv_font_t base = make_font(), wrapper = make_font(), other = make_font();
    lv_cache_entry_t *a = NULL, *b = NULL, *c = NULL;
    assert(!ui_reader_font_derived_store(&wrapper, 65, 1, source_pixels, &a));
    assert(ui_reader_font_cache_attach(&base));

    lv_font_glyph_dsc_t original;
    assert(lv_font_get_glyph_dsc(&base, &original, 65, 0));
    const lv_draw_buf_t *source = lv_font_get_glyph_bitmap(&original, NULL);
    assert(source && original.entry && source_releases == 1);
    const lv_draw_buf_t *pixels = epd_app_font_derived_store(&wrapper, 65, 1, source, &a);
    assert(pixels && a && pixels != source && !memcmp(pixels->data, source->data, 64));
    assert(lv_cache_entry_get_ref(original.entry) == 1);
    unsigned before = copies;
    assert(epd_app_font_derived_acquire(&wrapper, 65, 1, &b) == pixels);
    assert(a == b && copies == before && lv_cache_entry_get_ref(a) == 2);
    assert(ui_reader_font_derived_store(&wrapper, 65, 1, source, &c) == pixels);
    assert(copies == before);
    release_entry(&c);
    assert(!ui_reader_font_derived_acquire(&wrapper, 66, 1, &c));
    assert(!ui_reader_font_derived_acquire(&wrapper, 65, 2, &c));
    assert(!ui_reader_font_derived_acquire(&other, 65, 1, &c));
    assert(ui_reader_font_derived_store(&wrapper, 65, 2, source, &c));
    release_entry(&c);
    assert(ui_reader_font_derived_store(&other, 65, 1, source, &c));
    release_entry(&c);
    /* Derived style zero cannot alias the base-font key. */
    assert(ui_reader_font_derived_store(&base, 65, 0, source, &c) != source);
    release_entry(&c);

    /* Invalidation preserves references, removes every style of only one font. */
    unsigned old_waits = waits;
    epd_app_font_derived_drop(&wrapper);
    assert(waits == old_waits + 1 && lv_cache_entry_is_invalid(a));
    assert(!ui_reader_font_derived_acquire(&wrapper, 65, 1, &c));
    assert(!ui_reader_font_derived_acquire(&wrapper, 65, 2, &c));
    assert(ui_reader_font_derived_acquire(&other, 65, 1, &c));
    release_entry(&c);
    assert(pixels->data[0] == 0x3a);
    epd_app_font_derived_release(a);
    a = NULL;
    release_entry(&b);

    source_pixels->data[0] = 0x91;
    assert(ui_reader_font_derived_store(&wrapper, 65, 1, source_pixels, &a)->data[0] == 0x91);
    release_entry(&a);
    fail_pixels = true;
    assert(!ui_reader_font_derived_store(&wrapper, 67, 1, source_pixels, &a) && !a);
    assert(source_pixels->data[0] == 0x91);
    fail_pixels = false;
    assert(ui_reader_font_derived_store(&wrapper, 67, 1, source_pixels, &a));
    release_entry(&a);
    lv_draw_buf_t malformed = *source_pixels;
    malformed.data_size = 1;
    assert(!ui_reader_font_derived_store(&wrapper, 68, 1, &malformed, &a));

    /* Shared LRU pressure cannot evict acquired source or derived pixels. */
    pixels = ui_reader_font_derived_acquire(&wrapper, 67, 1, &a);
    assert(pixels);
    available = 0;
    ui_reader_font_cache_reserve(1);
    assert(lv_cache_entry_get_ref(original.entry) == 1 && source->data[0] == 0x3a);
    assert(pixels->data[0] == 0x91);
    assert(!ui_reader_font_derived_store(&other, 70, 1, source_pixels, &b));
    available = 64u * 1024u * 1024u;
    lv_font_glyph_release_draw_data(&original);
    release_entry(&a);

    lv_draw_buf_t *large = lv_draw_buf_create(1024, 1024, LV_COLOR_FORMAT_A8, 1024);
    assert(large);
    memset(large->data, 0x54, large->data_size);
    for (uint32_t i = 0; i < 12; ++i)
    {
        assert(ui_reader_font_derived_store(&wrapper, 100 + i, 1, large, &a));
        release_entry(&a);
        assert(glyph_bytes <= GLYPH_BUDGET && glyph_count <= GLYPH_NODE_LIMIT);
    }
    assert(!ui_reader_font_derived_acquire(&wrapper, 100, 1, &a));
    assert(ui_reader_font_derived_acquire(&wrapper, 111, 1, &a));
    release_entry(&a);
    ui_reader_font_derived_drop(&wrapper);
    for (uint32_t i = 0; i < GLYPH_NODE_LIMIT + 2; ++i)
    {
        assert(ui_reader_font_derived_store(&wrapper, 1000 + i, 1, source_pixels, &a));
        release_entry(&a);
    }
    assert(glyph_count <= GLYPH_NODE_LIMIT);
    assert(!ui_reader_font_derived_acquire(&wrapper, 1000, 1, &a));
    ui_reader_font_derived_drop(&wrapper);

    /* Held invalidated entries still consume the common budget. */
    lv_cache_entry_t *held[7];
    for (uint32_t i = 0; i < 7; ++i)
        assert(ui_reader_font_derived_store(&wrapper, 200 + i, 1, large, &held[i]));
    ui_reader_font_derived_drop(&wrapper);
    assert(!ui_reader_font_derived_store(&wrapper, 220, 1, large, &a));
    for (unsigned i = 0; i < 7; ++i) release_entry(&held[i]);
    assert(ui_reader_font_derived_store(&wrapper, 220, 1, large, &a));
    ui_reader_font_cache_detach(&base);
    assert(lv_cache_entry_is_invalid(a) && glyph_cache);
    release_entry(&a);
    assert(!glyph_cache && !glyph_items && !glyph_bytes && !glyph_count);
    assert(copies == frees && copies == flushes);
    assert(base.get_glyph_bitmap == source_bitmap && !test_mutex_depth);
    assert(ui_reader_font_cache_attach(&base));
    ui_reader_font_cache_detach(&base);
    lv_draw_buf_destroy(large);
    lv_draw_buf_destroy(source_pixels);
    lv_deinit();
    puts("derived cache: real LVGL LRU, key identity, source pins, pressure, failure, drop and lifetime passed");
    return 0;
}
