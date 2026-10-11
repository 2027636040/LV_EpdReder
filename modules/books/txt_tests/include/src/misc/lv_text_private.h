#ifndef TXT_TEST_TEXT_H
#define TXT_TEST_TEXT_H
#include "lvgl.h"
typedef struct { int max_width, line_space; } lv_text_attributes_t;
void lv_text_attributes_init(lv_text_attributes_t *);
unsigned lv_text_get_next_line(const char *, unsigned, const lv_font_t *, void *, const lv_text_attributes_t *);
#endif
