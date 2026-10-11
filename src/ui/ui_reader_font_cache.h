#ifndef UI_READER_FONT_CACHE_H
#define UI_READER_FONT_CACHE_H
#include "lvgl.h"

/* UI-thread ownership; generated bitmaps retain LVGL cache references. */
bool ui_reader_font_cache_attach(lv_font_t *font);
void ui_reader_font_cache_detach(lv_font_t *font);
/* UI thread only: reclaim speculative pixels before a large shared allocation. */
void ui_reader_font_cache_reserve(size_t bytes);
bool ui_reader_font_prepare(const lv_font_t *font, uint32_t letter, bool adjacent);
/* Shared pixel LRU. A successful acquire/store returns one owned reference.
 * store copies an A8 bitmap; a miss/allocation failure returns NULL and leaves
 * the caller's bitmap untouched. Callable from UI preparation/draw callbacks. */
const lv_draw_buf_t *ui_reader_font_derived_acquire(const lv_font_t *font, uint32_t glyph,
                                                  uint32_t style, lv_cache_entry_t **entry);
const lv_draw_buf_t *ui_reader_font_derived_store(const lv_font_t *font, uint32_t glyph,
                                                uint32_t style, const lv_draw_buf_t *bitmap,
                                                lv_cache_entry_t **entry);
void ui_reader_font_derived_release(lv_cache_entry_t *entry);
/* UI thread only: wait for drawing, then invalidate this wrapper's pixels. */
void ui_reader_font_derived_drop(const lv_font_t *font);
#endif
