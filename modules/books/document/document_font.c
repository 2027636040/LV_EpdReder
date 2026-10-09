/* SPDX-License-Identifier: Apache-2.0 */
#include "document_font.h"
#include "platform/epd_app.h"
#include <string.h>

typedef struct { lv_font_t font; const lv_font_t *source; } oblique_font_t;
typedef struct { lv_draw_buf_t buffer; uint8_t pixels[]; } oblique_bitmap_t;

static bool descriptor(const lv_font_t *font, lv_font_glyph_dsc_t *glyph, uint32_t letter, uint32_t next)
{
    (void)next;
    const oblique_font_t *style = (const oblique_font_t *)font;
    bool found = lv_font_get_glyph_dsc(style->source, glyph, letter, 0);
    unsigned shear = glyph->box_h > 1 ? (glyph->box_h - 1) / 4 : 0;
    glyph->box_w += shear;
    glyph->adv_w += shear;
    glyph->stride = RT_ALIGN(glyph->box_w, 4);
    glyph->format = LV_FONT_GLYPH_FORMAT_A8;
    glyph->gid.index = letter;
    glyph->entry = NULL;
    return found;
}

static const void *bitmap(lv_font_glyph_dsc_t *glyph, lv_draw_buf_t *scratch)
{
    const oblique_font_t *style = (const oblique_font_t *)glyph->resolved_font;
    lv_font_glyph_dsc_t original;
    if (!lv_font_get_glyph_dsc(style->source, &original, glyph->gid.index, 0)) return NULL;
    const lv_draw_buf_t *source = lv_font_get_glyph_bitmap(&original, scratch);
    if (!source || source->header.cf != LV_COLOR_FORMAT_A8)
    { lv_font_glyph_release_draw_data(&original); return NULL; }
    unsigned width = glyph->box_w, height = glyph->box_h, stride = RT_ALIGN(width, 4);
    oblique_bitmap_t *out = epd_app_alloc(sizeof(*out) + stride * height + 31, EPD_APP_PSRAM);
    if (!out) { lv_font_glyph_release_draw_data(&original); return NULL; }
    memset(out, 0, sizeof(*out));
    out->buffer.header.magic = LV_IMAGE_HEADER_MAGIC;
    out->buffer.header.cf = LV_COLOR_FORMAT_A8;
    out->buffer.header.w = width; out->buffer.header.h = height; out->buffer.header.stride = stride;
    out->buffer.data = (uint8_t *)RT_ALIGN((uintptr_t)out->pixels, 32);
    out->buffer.data_size = stride * height;
    memset(out->buffer.data, 0, stride * height);
    unsigned rows = source->header.h < height ? source->header.h : height;
    for (unsigned y = 0; y < rows; ++y)
    {
        unsigned shift = (height - y - 1) / 4;
        unsigned n = source->header.w < width - shift ? source->header.w : width - shift;
        memcpy(out->buffer.data + y * stride + shift, source->data + y * source->header.stride, n);
    }
    lv_font_glyph_release_draw_data(&original);
    epd_app_clean_draw_buffer(&out->buffer);
    glyph->entry = (lv_cache_entry_t *)out;
    return &out->buffer;
}
static void release(const lv_font_t *font, lv_font_glyph_dsc_t *glyph)
{
    (void)font;
    epd_app_free(glyph->entry);
    glyph->entry = NULL;
}
lv_font_t *document_font_oblique(const lv_font_t *source)
{
    oblique_font_t *style = epd_app_alloc(sizeof(*style), EPD_APP_PSRAM);
    if (!style) return NULL;
    memset(style, 0, sizeof(*style));
    style->source = source;
    style->font.line_height = source->line_height;
    style->font.base_line = source->base_line;
    style->font.underline_position = source->underline_position;
    style->font.underline_thickness = source->underline_thickness;
    style->font.kerning = LV_FONT_KERNING_NONE;
    style->font.get_glyph_dsc = descriptor;
    style->font.get_glyph_bitmap = bitmap;
    style->font.release_glyph = release;
    return &style->font;
}
void document_font_free(lv_font_t *font) { epd_app_free(font); }
