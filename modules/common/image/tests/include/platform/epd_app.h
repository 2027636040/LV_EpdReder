#ifndef RASTER_HOST_APP_H
#define RASTER_HOST_APP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define EPD_APP_SRAM 0
#define EPD_APP_PSRAM 1
#define RT_ALIGN(value, alignment) (((value) + (alignment) - 1) & ~((uintptr_t)(alignment) - 1))
typedef uint32_t rt_uint32_t;
typedef uint32_t rt_tick_t;
void *epd_app_alloc(size_t size, int pool);
void epd_app_free(void *pointer);
void storage_lock(void);
void storage_unlock(void);
bool storage_path_available(const char *path);
void rt_memory_info(rt_uint32_t *total, rt_uint32_t *used, rt_uint32_t *peak);
rt_tick_t rt_tick_get(void);
int rt_kprintf(const char *format, ...);
#endif
