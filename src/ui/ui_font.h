#ifndef UI_FONT_H
#define UI_FONT_H
#include "lvgl.h"

bool ui_font_init(void);
const lv_font_t *ui_font_small(void);
const lv_font_t *ui_font_body(void);
const lv_font_t *ui_font_title(void);
const lv_font_t *ui_font_caption(void);
const lv_font_t *ui_font_temperature(void);
lv_font_t *ui_font_reader_create(unsigned family, unsigned size, uint32_t *identity,
                                lv_font_t **fallback);

#endif
