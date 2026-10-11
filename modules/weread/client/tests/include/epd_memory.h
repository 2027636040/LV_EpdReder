#ifndef TEST_EPD_MEMORY_H
#define TEST_EPD_MEMORY_H
#include <stddef.h>
typedef enum { EPD_APP_SRAM, EPD_APP_PSRAM } epd_app_memory_t;
void *epd_app_alloc(size_t size, epd_app_memory_t region);
void epd_app_free(void *memory);
#endif
