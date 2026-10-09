/* SPDX-License-Identifier: Apache-2.0 */
#include "epd_memory.h"
#include <rthw.h>

/* Share the SDK heap also used by images and the dynamic module loader. */
extern struct rt_memheap app_image_psram_memheap;

void *epd_app_alloc(size_t size, epd_app_memory_t region)
{
    if (!size)
    {
        return RT_NULL;
    }
    return region == EPD_APP_PSRAM ?
           rt_memheap_alloc(&app_image_psram_memheap, size) : rt_malloc(size);
}

void *epd_app_realloc(void *memory, size_t size, epd_app_memory_t region)
{
    if (!size)
    {
        epd_app_free(memory);
        return RT_NULL;
    }
    if (!memory)
    {
        return epd_app_alloc(size, region);
    }
    if (region == EPD_APP_PSRAM)
    {
        return rt_memheap_realloc(&app_image_psram_memheap, memory, size);
    }
    return rt_realloc(memory, size);
}

void epd_app_free(void *memory)
{
    /* RT_USING_MEMHEAP_AS_HEAP: each allocation records its owning heap. */
    rt_free(memory);
}

size_t epd_app_psram_available(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    size_t available = app_image_psram_memheap.available_size;
    rt_hw_interrupt_enable(level);
    return available;
}

RTM_EXPORT(epd_app_alloc);
RTM_EXPORT(epd_app_realloc);
RTM_EXPORT(epd_app_free);
RTM_EXPORT(epd_app_psram_available);
