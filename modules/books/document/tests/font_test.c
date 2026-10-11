/* Host checks for derived-glyph references and the uncached oblique fallback. */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "platform/epd_app.h"
#include "../../../common/reader/reader_font.c"

struct lv_cache_entry {
    lv_draw_buf_t buffer;
    const lv_font_t *font;
    uint32_t glyph, style;
    unsigned references;
    bool invalid;
};
static lv_cache_entry_t *cached_entry;
static lv_font_t source_font = {.line_height = 12};
static uint8_t source_pixels[32];
static lv_draw_buf_t source_bitmap;
static unsigned base_references, source_renders, acquire_hits, stores, drops, waits, allocations;
static bool cache_enabled = true, missing;
static int fail_after = -1;
void *epd_app_alloc(size_t size, int region)
{
    (void)region;
    if (!fail_after) return NULL;
    if (fail_after > 0) --fail_after;
    void *p = malloc(size); if (p) ++allocations; return p;
}
void epd_app_free(void *p) { if (p) { assert(allocations); --allocations; free(p); } }
void lv_draw_wait_for_finish(void) { ++waits; }
void epd_app_clean_draw_buffer(lv_draw_buf_t *buffer) { assert(buffer->data && buffer->data_size); }
bool lv_font_get_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *glyph, uint32_t letter, uint32_t next)
{
    bool result;
    if (font->get_glyph_dsc) result = font->get_glyph_dsc(font, glyph, letter, next);
    else
    {
        memset(glyph, 0, sizeof(*glyph)); glyph->adv_w = 4; glyph->box_w = 4; glyph->box_h = 8;
        glyph->stride = 4; glyph->format = LV_FONT_GLYPH_FORMAT_A8; glyph->gid.index = letter;
        result = !missing;
    }
    glyph->resolved_font = font; return result;
}
const void *lv_font_get_glyph_bitmap(lv_font_glyph_dsc_t *glyph, lv_draw_buf_t *scratch)
{
    if (glyph->resolved_font->get_glyph_bitmap) return glyph->resolved_font->get_glyph_bitmap(glyph, scratch);
    ++base_references; ++source_renders; return &source_bitmap;
}
void lv_font_glyph_release_draw_data(lv_font_glyph_dsc_t *glyph)
{
    if (glyph->resolved_font->release_glyph) glyph->resolved_font->release_glyph(glyph->resolved_font, glyph);
    else { assert(base_references); --base_references; }
}
static void destroy_entry(void)
{
    assert(cached_entry && !cached_entry->references);
    free(cached_entry->buffer.data); free(cached_entry); cached_entry = NULL;
}
const lv_draw_buf_t *epd_app_font_derived_acquire(const lv_font_t *font, uint32_t glyph,
                                                uint32_t style, lv_cache_entry_t **entry)
{
    *entry = NULL;
    if (!cache_enabled || !cached_entry || cached_entry->invalid ||
        cached_entry->font != font || cached_entry->glyph != glyph || cached_entry->style != style) return NULL;
    *entry = cached_entry; ++cached_entry->references; ++acquire_hits; return &cached_entry->buffer;
}
const lv_draw_buf_t *epd_app_font_derived_store(const lv_font_t *font, uint32_t glyph,
                                              uint32_t style, const lv_draw_buf_t *bitmap,
                                              lv_cache_entry_t **entry)
{
    assert(base_references == 1);
    *entry = NULL;
    if (!cache_enabled || cached_entry) return NULL;
    cached_entry = calloc(1, sizeof(*cached_entry)); assert(cached_entry);
    cached_entry->font = font; cached_entry->glyph = glyph; cached_entry->style = style;
    cached_entry->buffer = *bitmap;
    cached_entry->buffer.data = malloc(bitmap->data_size); assert(cached_entry->buffer.data);
    memcpy(cached_entry->buffer.data, bitmap->data, bitmap->data_size);
    cached_entry->references = 1; *entry = cached_entry; ++stores;
    return &cached_entry->buffer;
}
void epd_app_font_derived_release(lv_cache_entry_t *entry)
{
    assert(entry == cached_entry && entry->references); --entry->references;
    if (!entry->references && entry->invalid) destroy_entry();
}
void epd_app_font_derived_drop(const lv_font_t *font)
{
    assert(waits); ++drops;
    if (cached_entry && cached_entry->font == font)
    { cached_entry->invalid = true; if (!cached_entry->references) destroy_entry(); }
}
static const lv_draw_buf_t *render(lv_font_t *font, lv_font_glyph_dsc_t *glyph)
{
    assert(lv_font_get_glyph_dsc(font, glyph, 'A', 0));
    assert(glyph->box_w == 5 && glyph->adv_w == 5 && glyph->stride == 8);
    return lv_font_get_glyph_bitmap(glyph, NULL);
}
static void check_pixels(const lv_draw_buf_t *bitmap)
{
    assert(bitmap && bitmap->header.w == 5 && bitmap->header.h == 8 && bitmap->header.stride == 8);
    for (unsigned y = 0; y < 8; ++y)
        for (unsigned x = 0; x < 8; ++x)
        {
            unsigned shift = (7 - y) / 4;
            unsigned char expected = x >= shift && x < shift + 4 ? source_pixels[y * 4 + x - shift] : 0;
            assert(bitmap->data[y * 8 + x] == expected);
        }
}
int main(void)
{
    for (unsigned i = 0; i < sizeof(source_pixels); ++i) source_pixels[i] = i + 1;
    source_bitmap.data = source_pixels; source_bitmap.data_size = sizeof(source_pixels);
    source_bitmap.header.cf = LV_COLOR_FORMAT_A8;
    source_bitmap.header.w = source_bitmap.header.stride = 4; source_bitmap.header.h = 8;
    lv_font_t *font = document_font_oblique(&source_font); assert(font);
    lv_font_glyph_dsc_t first, second;
    check_pixels(render(font, &first));
    assert(stores == 1 && !base_references && cached_entry->references == 1);
    check_pixels(render(font, &second));
    assert(acquire_hits == 1 && source_renders == 1 && cached_entry->references == 2);
    lv_font_glyph_release_draw_data(&first); lv_font_glyph_release_draw_data(&second);
    assert(!cached_entry->references && allocations == 1);
    cache_enabled = false;
    check_pixels(render(font, &first)); assert(!base_references);
    oblique_reference_t *reference = (oblique_reference_t *)first.entry;
    assert(reference->immediate && !reference->cached);
    lv_font_glyph_release_draw_data(&first); assert(allocations == 1);
    for (int failure = 0; failure <= 1; ++failure)
    {
        fail_after = failure;
        assert(!render(font, &first) && !base_references && !first.entry);
        fail_after = -1; assert(allocations == 1);
    }
    source_bitmap.header.cf = LV_COLOR_FORMAT_RGB565;
    assert(!render(font, &first) && !base_references && allocations == 1);
    source_bitmap.header.cf = LV_COLOR_FORMAT_A8;
    cache_enabled = true;
    check_pixels(render(font, &first));
    document_font_free(font);
    assert(drops == 1 && cached_entry && cached_entry->invalid && cached_entry->references == 1);
    /* A dropped cache entry owns its pixels independently of the wrapper. */
    release(NULL, &first);
    assert(!cached_entry && !allocations && !base_references);
    document_font_free(NULL);
    puts("PASS: oblique shared-cache hit, held source reference, immediate fallback, allocation failure, drop-before-free");
    return 0;
}
