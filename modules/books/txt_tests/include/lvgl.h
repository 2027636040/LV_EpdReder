#ifndef TXT_TEST_LVGL_H
#define TXT_TEST_LVGL_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef struct lv_font lv_font_t;
typedef struct { unsigned adv_w; bool is_placeholder; } lv_font_glyph_dsc_t;
struct lv_font {
    int line_height, kerning;
    bool (*get_glyph_dsc)(const lv_font_t *, lv_font_glyph_dsc_t *, uint32_t, uint32_t);
    void *user_data;
};
typedef struct lv_obj lv_obj_t;
typedef struct { unsigned width, height; const void *data; } lv_image_dsc_t;
#define LV_FONT_KERNING_NONE 0
#define LV_LOG_USER(...) ((void)0)
bool lv_font_get_glyph_dsc(const lv_font_t *, lv_font_glyph_dsc_t *, uint32_t, uint32_t);
void lv_draw_wait_for_finish(void);
#endif
