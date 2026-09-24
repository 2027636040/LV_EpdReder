#include "ui_font.h"
#include <sys/stat.h>

#ifndef UI_BUILTIN_FONT_PATH
extern const unsigned char epub_ttf_data[];
extern const unsigned int epub_ttf_data_size;
#endif

static lv_font_t *fonts[5];

typedef struct
{
    unsigned weight;
    bool (*descriptor)(const lv_font_t *, lv_font_glyph_dsc_t *, uint32_t, uint32_t);
    const void *(*bitmap)(lv_font_glyph_dsc_t *, lv_draw_buf_t *);
} reader_weight_t;

static bool weighted_descriptor(const lv_font_t *font, lv_font_glyph_dsc_t *glyph,
                                uint32_t letter, uint32_t next)
{
    const reader_weight_t *style = font->user_data;
    if (!style->descriptor(font, glyph, letter, next)) return false;
    /* Reserve the extra ink width in both line measurement and drawing. */
    if (style->weight == 1 && glyph->box_w > 1 && glyph->box_h > 1) ++glyph->adv_w;
    return true;
}

static const void *weighted_bitmap(lv_font_glyph_dsc_t *glyph, lv_draw_buf_t *scratch)
{
    const reader_weight_t *style = glyph->resolved_font->user_data;
    lv_draw_buf_t *buffer = (lv_draw_buf_t *)style->bitmap(glyph, scratch);
    if (!buffer || (buffer->header.flags & LV_IMAGE_FLAGS_USER1) ||
        buffer->header.cf != LV_COLOR_FORMAT_A8) return buffer;
    /* Each reader font owns its Tiny TTF cache. Process each new bitmap once;
     * never repeatedly embolden an already cached glyph. */
    for (uint32_t y = 0; y < buffer->header.h; ++y)
    {
        uint8_t *row = buffer->data + y * buffer->header.stride;
        uint8_t previous = 0;
        for (uint32_t x = 0; x < buffer->header.w; ++x)
        {
            uint8_t current = row[x];
            if (style->weight == 1)
                row[x] = current > previous ? current : previous;
            else
            {
                uint8_t next = x + 1 < buffer->header.w ? row[x + 1] : 0;
                uint8_t edge = previous < next ? previous : next;
                if (edge > current) edge = current;
                /* Half-pixel horizontal erosion preserves one-pixel strokes. */
                row[x] = ((unsigned)current + edge + 1) / 2;
            }
            previous = current;
        }
    }
    buffer->header.flags |= LV_IMAGE_FLAGS_USER1;
    /* Includes stride padding; the registered handler writes back PSRAM. */
    lv_draw_buf_flush_cache(buffer, NULL);
    return buffer;
}

static bool reader_weight_apply(lv_font_t *font, unsigned weight)
{
    if (!weight) return true;
    reader_weight_t *style = lv_malloc(sizeof(*style));
    if (!style) return false;
    style->weight = weight;
    style->descriptor = font->get_glyph_dsc;
    style->bitmap = font->get_glyph_bitmap;
    font->user_data = style;
    font->get_glyph_dsc = weighted_descriptor;
    font->get_glyph_bitmap = weighted_bitmap;
    return true;
}

void ui_font_reader_destroy(lv_font_t *font)
{
    if (!font) return;
    if (font->get_glyph_bitmap == weighted_bitmap) lv_free(font->user_data);
    lv_tiny_ttf_destroy(font);
}

bool ui_font_init(void)
{
    static const uint32_t sizes[] = {24, 28, 36, 20, 64};
    static const lv_font_t *fallbacks[] = {
        &lv_font_montserrat_24, &lv_font_montserrat_28, &lv_font_montserrat_36,
        &lv_font_montserrat_20, &lv_font_montserrat_36
    };
    if (fonts[0]) return true;
    for (unsigned i = 0; i < sizeof(fonts) / sizeof(fonts[0]); ++i)
    {
#ifdef UI_BUILTIN_FONT_PATH
        fonts[i] = lv_tiny_ttf_create_file_ex(UI_BUILTIN_FONT_PATH, sizes[i],
                    LV_FONT_KERNING_NONE, LV_TINY_TTF_CACHE_GLYPH_CNT);
#else
        fonts[i] = lv_tiny_ttf_create_data_ex(epub_ttf_data, epub_ttf_data_size, sizes[i],
                    LV_FONT_KERNING_NONE, LV_TINY_TTF_CACHE_GLYPH_CNT);
#endif
        if (!fonts[i])
        {
            while (i) lv_tiny_ttf_destroy(fonts[--i]);
            for (i = 0; i < sizeof(fonts) / sizeof(fonts[0]); ++i) fonts[i] = NULL;
            return false;
        }
        fonts[i]->fallback = fallbacks[i];
    }
    return true;
}

const lv_font_t *ui_font_small(void) { return fonts[0]; }
const lv_font_t *ui_font_body(void) { return fonts[1]; }
const lv_font_t *ui_font_title(void) { return fonts[2]; }
const lv_font_t *ui_font_caption(void) { return fonts[3]; }
const lv_font_t *ui_font_temperature(void) { return fonts[4]; }

lv_font_t *ui_font_reader_create(unsigned family, unsigned size, unsigned weight, uint32_t *identity,
                                lv_font_t **fallback)
{
    static const char *const paths[] = {NULL, "/fonts/Song.ttf", "/fonts/Hei.ttf",
                                        "/fonts/Kai.ttf", "/fonts/Monospace.ttf"};
    lv_font_t *font = NULL;
    *fallback = NULL;
    if (weight > 2) weight = 0;
    *identity = size ^ (weight << 20);
    if (family > 0 && family < sizeof(paths) / sizeof(paths[0]))
    {
        struct stat st;
        if (stat(paths[family], &st) == 0)
        {
            font = lv_tiny_ttf_create_file_ex(paths[family], size, LV_FONT_KERNING_NONE, LV_TINY_TTF_CACHE_GLYPH_CNT);
            if (font) *identity ^= (uint32_t)st.st_size ^ (uint32_t)st.st_mtime ^ (family << 24);
        }
    }
#ifdef UI_BUILTIN_FONT_PATH
    lv_font_t *builtin = lv_tiny_ttf_create_file_ex(UI_BUILTIN_FONT_PATH, size,
                                                  LV_FONT_KERNING_NONE, LV_TINY_TTF_CACHE_GLYPH_CNT);
#else
    lv_font_t *builtin = lv_tiny_ttf_create_data_ex(epub_ttf_data, epub_ttf_data_size, size,
                                                  LV_FONT_KERNING_NONE, LV_TINY_TTF_CACHE_GLYPH_CNT);
#endif
    if (!builtin)
    {
        if (font) lv_tiny_ttf_destroy(font);
        return NULL;
    }
    builtin->fallback = &lv_font_montserrat_16;
    if (font)
    {
        font->fallback = builtin;
        *fallback = builtin;
    }
    else font = builtin;
    if (!reader_weight_apply(font, weight) || (*fallback && !reader_weight_apply(*fallback, weight)))
    {
        ui_font_reader_destroy(font);
        ui_font_reader_destroy(*fallback);
        *fallback = NULL;
        return NULL;
    }
    return font;
}
