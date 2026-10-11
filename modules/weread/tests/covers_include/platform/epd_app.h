#ifndef COVERS_TEST_EPD_APP_H
#define COVERS_TEST_EPD_APP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#define STORAGE_PATH_MAX 512
#define EPD_APP_SRAM 0
#define EPD_APP_PSRAM 1
#define RT_TICK_PER_SECOND 1000
#define RT_ALIGN(value, alignment) (((value) + (alignment) - 1) & ~((uintptr_t)(alignment) - 1))
#define LV_IMAGE_HEADER_MAGIC 0x19
#define LV_COLOR_FORMAT_RGB565 1
typedef int rt_base_t;
typedef uint32_t rt_tick_t, rt_uint32_t;
typedef struct { unsigned magic, cf, w, h, stride; } lv_image_header_t;
typedef struct { lv_image_header_t header; size_t data_size; const uint8_t *data; } lv_image_dsc_t;
typedef struct { uint8_t *data; size_t data_size; } lv_draw_buf_t;
typedef enum { STORAGE_APP_CODE, STORAGE_APP_DATA, STORAGE_APP_CACHE } storage_app_area_t;
typedef struct epd_service epd_service_t;
void *epd_app_alloc(size_t size, int region);
void epd_app_free(void *pointer);
void epd_app_clean_draw_buffer(lv_draw_buf_t *buffer);
void lv_draw_wait_for_finish(void);
void lv_image_cache_drop(const void *source);
void lv_image_header_cache_drop(const void *source);
uint32_t storage_crc32(const void *data, size_t size);
void storage_lock(void);
void storage_unlock(void);
bool storage_path_available(const char *path);
void rt_memory_info(rt_uint32_t *total, rt_uint32_t *used, rt_uint32_t *peak);
rt_tick_t rt_tick_get(void);
int rt_kprintf(const char *format, ...);
#endif
