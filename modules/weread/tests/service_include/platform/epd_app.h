/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SERVICE_TEST_EPD_APP_H
#define SERVICE_TEST_EPD_APP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define STORAGE_PATH_MAX 512
#define EPD_APP_PSRAM 1
#define RT_WAITING_FOREVER (-1)
typedef int rt_base_t;
typedef uint32_t rt_tick_t;
/* Service tests do not render or inspect the cover image descriptor. */
typedef struct { const void *data; } lv_image_dsc_t;
typedef enum { STORAGE_APP_CODE, STORAGE_APP_DATA, STORAGE_APP_CACHE } storage_app_area_t;
typedef struct epd_service epd_service_t;
typedef struct
{
    void (*run)(epd_service_t *service);
    unsigned stack_size, priority;
    bool network;
    bool (*ui_process)(epd_service_t *service, bool stopping);
} epd_background_t;
void *epd_app_alloc(size_t size, int region);
void epd_app_free(void *pointer);
void rt_kprintf(const char *format, ...);
bool network_ready(void);
bool storage_app_available(const char *app);
epd_service_t *app_service_start(const char *app, const epd_background_t *definition, void *module);
void app_service_stop(const char *app);
bool epd_service_cancelled(epd_service_t *service);
void epd_service_wake(epd_service_t *service);
uint32_t epd_service_wait(epd_service_t *service, int ticks);
static inline rt_tick_t rt_tick_from_millisecond(int ms) { return (rt_tick_t)ms; }
#endif
