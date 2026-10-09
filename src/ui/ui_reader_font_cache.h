#ifndef UI_READER_FONT_CACHE_H
#define UI_READER_FONT_CACHE_H
#include "lvgl.h"

/* UI-thread ownership; generated bitmaps retain LVGL cache references. */
bool ui_reader_font_cache_attach(lv_font_t *font);
void ui_reader_font_cache_detach(lv_font_t *font);
/* UI thread only: reclaim speculative pixels before a large shared allocation. */
void ui_reader_font_cache_reserve(size_t bytes);
bool ui_reader_font_prepare(const lv_font_t *font, uint32_t letter, bool adjacent);
#endif
