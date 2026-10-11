#ifndef TEST_LVGL_H
#define TEST_LVGL_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef struct lv_font lv_font_t;
typedef struct lv_cache_entry lv_cache_entry_t;
typedef struct lv_draw_buf lv_draw_buf_t;
typedef struct {
    unsigned adv_w, box_w, box_h, stride, format;
    union { uint32_t index; } gid;
    lv_cache_entry_t *entry;
    const lv_font_t *resolved_font;
} lv_font_glyph_dsc_t;
struct lv_font {
    int line_height, kerning;
    int base_line, underline_position, underline_thickness;
    bool (*get_glyph_dsc)(const lv_font_t *, lv_font_glyph_dsc_t *, uint32_t, uint32_t);
    const void *(*get_glyph_bitmap)(lv_font_glyph_dsc_t *, lv_draw_buf_t *);
    void (*release_glyph)(const lv_font_t *, lv_font_glyph_dsc_t *);
};
typedef struct lv_obj { int unused; } lv_obj_t;
typedef struct lv_event { int unused; } lv_event_t;
typedef struct {
    struct { unsigned magic, cf, w, h, stride; } header;
    const uint8_t *data;
    unsigned data_size;
} lv_image_dsc_t;
struct lv_draw_buf {
    uint8_t *data; unsigned data_size;
    struct { unsigned magic, cf, w, h, stride; } header;
};
#define LV_IMAGE_HEADER_MAGIC 25
#define LV_COLOR_FORMAT_RGB565 1
#define LV_COLOR_FORMAT_A8 2
#define LV_FONT_GLYPH_FORMAT_A8 2
#define LV_FONT_KERNING_NONE 0
#define LV_OPA_COVER 255
#define LV_OBJ_FLAG_SCROLLABLE 1
#define LV_OBJ_FLAG_CLICKABLE 2
#define LV_OBJ_FLAG_HIDDEN 4
#define LV_LABEL_LONG_CLIP 0
#define LV_TEXT_DECOR_UNDERLINE 1
#define LV_TEXT_DECOR_NONE 0
#define LV_EVENT_CLICKED 1
#define LV_EVENT_DELETE 2
bool lv_font_get_glyph_dsc(const lv_font_t *, lv_font_glyph_dsc_t *, uint32_t, uint32_t);
const void *lv_font_get_glyph_bitmap(lv_font_glyph_dsc_t *, lv_draw_buf_t *);
void lv_font_glyph_release_draw_data(lv_font_glyph_dsc_t *);
void lv_draw_wait_for_finish(void);
lv_obj_t *lv_obj_create(lv_obj_t *);
lv_obj_t *lv_image_create(lv_obj_t *);
lv_obj_t *lv_label_create(lv_obj_t *);
lv_obj_t *lv_event_get_target_obj(lv_event_t *);
void *lv_event_get_user_data(lv_event_t *);
void lv_obj_delete(lv_obj_t *);
void lv_obj_set_size(lv_obj_t *, int, int);
void lv_obj_set_pos(lv_obj_t *, int, int);
void lv_obj_set_style_pad_all(lv_obj_t *, int, int);
void lv_obj_set_style_border_width(lv_obj_t *, int, int);
void lv_obj_set_style_bg_opa(lv_obj_t *, int, int);
void lv_obj_set_style_bg_color(lv_obj_t *, int, int);
void lv_obj_set_style_text_color(lv_obj_t *, int, int);
void lv_obj_set_style_text_font(lv_obj_t *, const lv_font_t *, int);
void lv_obj_set_style_text_line_space(lv_obj_t *, int, int);
void lv_obj_set_style_text_letter_space(lv_obj_t *, int, int);
void lv_obj_set_style_text_decor(lv_obj_t *, int, int);
void lv_obj_remove_flag(lv_obj_t *, unsigned);
void lv_obj_add_flag(lv_obj_t *, unsigned);
void lv_obj_add_event_cb(lv_obj_t *, void (*)(lv_event_t *), int, void *);
void lv_label_set_long_mode(lv_obj_t *, int);
void lv_label_set_text(lv_obj_t *, const char *);
void lv_label_set_text_static(lv_obj_t *, const char *);
void lv_image_set_src(lv_obj_t *, const void *);
int lv_color_white(void);
int lv_color_black(void);
#endif
