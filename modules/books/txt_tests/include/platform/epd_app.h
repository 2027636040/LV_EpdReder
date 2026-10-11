#ifndef TXT_TEST_EPD_APP_H
#define TXT_TEST_EPD_APP_H
#include "lvgl.h"
#include "rtthread.h"
typedef struct epd_service epd_service_t;
#define EPD_APP_PSRAM 1
void *epd_app_alloc(size_t, int);
void *epd_app_realloc(void *, size_t, int);
void epd_app_free(void *);
lv_font_t *epd_app_font_create(unsigned, unsigned, unsigned, uint32_t *, lv_font_t **);
void epd_app_font_destroy(lv_font_t *);
bool epd_app_font_prepare(const lv_font_t *, uint32_t, bool);
void epd_app_set_recent_reading(const char *, int);
#endif
