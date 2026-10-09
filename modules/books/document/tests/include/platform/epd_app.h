#ifndef TEST_EPD_APP_H
#define TEST_EPD_APP_H
#include "lvgl.h"
#include "rtthread.h"
typedef struct epd_service epd_service_t;
#define EPD_APP_PSRAM 1
#define EPD_FONT_BODY 0
void *epd_app_alloc(size_t, int);
void *epd_app_realloc(void *, size_t, int);
void epd_app_free(void *);
lv_font_t *epd_app_font_create(unsigned, unsigned, unsigned, uint32_t *, lv_font_t **);
void epd_app_font_destroy(lv_font_t *);
const lv_font_t *epd_app_font_role(unsigned);
bool epd_app_refresh_done(void);
void epd_app_set_recent_reading(const char *, int);
void epd_app_object_init(lv_obj_t *);
void epd_app_clean_draw_buffer(lv_draw_buf_t *);
void *epd_app_library_open(const char *, const char *);
void *epd_app_library_symbol(void *, const char *);
void epd_app_library_close(void *);
size_t epd_app_psram_available(void);
bool epd_app_font_prepare(const lv_font_t *, uint32_t, bool);
bool epd_service_cancelled(epd_service_t *);
void epd_service_wake(epd_service_t *);
uint32_t epd_service_wait(epd_service_t *, rt_int32_t);
#define STORAGE_FLASH_ROOT "/flash"
#define STORAGE_SD_ROOT "/sdcard"
typedef enum { STORAGE_FLASH, STORAGE_SD, STORAGE_COUNT } storage_volume_t;
typedef struct { bool mounted; uint64_t total, used; } storage_info_t;
const char *storage_path_root(const char *);
bool storage_path_available(const char *);
bool storage_available(storage_volume_t);
uint32_t storage_revision(void);
uint32_t storage_card_session(void);
typedef enum { STORAGE_APP_CODE, STORAGE_APP_DATA, STORAGE_APP_CACHE } storage_app_area_t;
bool storage_app_path(char *, size_t, const char *, storage_app_area_t, const char *);
bool storage_changing(void);
void storage_lock(void);
void storage_unlock(void);
bool storage_mkdirs(const char *);
bool storage_info(storage_volume_t, storage_info_t *);
bool storage_file_signature(const char *, uint32_t *);
#endif
