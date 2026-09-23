#include "ui_font.h"
#include <sys/stat.h>

#ifndef UI_BUILTIN_FONT_PATH
extern const unsigned char epub_ttf_data[];
extern const unsigned int epub_ttf_data_size;
#endif

static lv_font_t *fonts[5];

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

lv_font_t *ui_font_reader_create(unsigned family, unsigned size, uint32_t *identity,
                                lv_font_t **fallback)
{
    static const char *const paths[] = {NULL, "/fonts/Song.ttf", "/fonts/Hei.ttf",
                                        "/fonts/Kai.ttf", "/fonts/Monospace.ttf"};
    lv_font_t *font = NULL;
    *fallback = NULL;
    *identity = size;
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
    return font;
}
