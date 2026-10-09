/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_MEMORY_H
#define EPD_MEMORY_H

#include <rtthread.h>

typedef enum { EPD_APP_SRAM, EPD_APP_PSRAM } epd_app_memory_t;

/* Thread-safe SDK heaps. PSRAM allocations never spill into scarce SRAM.
 * Realloc must use the original region. Free accepts NULL, but not pointers
 * returned by lv_malloc or the SDK's tagged app_cache_alloc. */
void *epd_app_alloc(size_t size, epd_app_memory_t region);
void *epd_app_realloc(void *memory, size_t size, epd_app_memory_t region);
void epd_app_free(void *memory);
size_t epd_app_psram_available(void);

#endif
