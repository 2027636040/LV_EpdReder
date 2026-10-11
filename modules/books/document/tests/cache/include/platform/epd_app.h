#ifndef CACHE_TEST_EPD_APP_H
#define CACHE_TEST_EPD_APP_H
#include "lvgl.h"
#include "rtthread.h"
#include "services/storage_file.h"
typedef struct epd_service epd_service_t;
#define EPD_APP_PSRAM 1
void *epd_app_alloc(size_t size, int region);
void *epd_app_realloc(void *memory, size_t size, int region);
void epd_app_free(void *memory);
#endif
