#include "raster.h"
#include "epd_image.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef union
{
    max_align_t alignment;
    struct { size_t bytes; int pool; } info;
} allocation_t;
static size_t live, peak, calls, fail_at;
static size_t sram_capacity = 256 * 1024, sram_live, sram_peak;
static bool fail_sram;
static unsigned cancel_reads, remove_reads;
static const raster_decoder_t *active_decoder;

void *epd_app_alloc(size_t size, int pool)
{
    ++calls;
    if ((fail_at && calls >= fail_at) || size > SIZE_MAX - sizeof(allocation_t)) return NULL;
    if (pool == EPD_APP_SRAM && (fail_sram || size > sram_capacity - sram_live)) return NULL;
    allocation_t *allocation = malloc(sizeof(*allocation) + size);
    if (!allocation) return NULL;
    allocation->info.bytes = size;
    allocation->info.pool = pool;
    if (pool == EPD_APP_SRAM)
    {
        sram_live += size;
        if (sram_live > sram_peak) sram_peak = sram_live;
    }
    live += size;
    if (live > peak) peak = live;
    return allocation + 1;
}

void epd_app_free(void *pointer)
{
    if (!pointer) return;
    allocation_t *allocation = (allocation_t *)pointer - 1;
    assert(live >= allocation->info.bytes);
    live -= allocation->info.bytes;
    if (allocation->info.pool == EPD_APP_SRAM)
    {
        assert(sram_live >= allocation->info.bytes);
        sram_live -= allocation->info.bytes;
    }
    free(allocation);
}

void storage_lock(void) {}
void storage_unlock(void) {}
bool storage_path_available(const char *path)
{ return path && (!remove_reads || active_decoder->read_calls < remove_reads); }
void rt_memory_info(rt_uint32_t *total, rt_uint32_t *used, rt_uint32_t *maximum)
{ *total = sram_capacity; *used = sram_live; *maximum = sram_peak; }
rt_tick_t rt_tick_get(void)
{
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (rt_tick_t)((uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000);
}
int rt_kprintf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int count = vfprintf(stderr, format, args);
    va_end(args);
    return count;
}

/* Host tests exercise software decoding; hardware belongs to the target HAL. */
bool epd_image_jpeg_layout(unsigned width, unsigned height, epd_jpeg_layout_t *layout)
{ (void)width; (void)height; (void)layout; return false; }
bool epd_image_jpeg_decode(const uint8_t *input, size_t length,
                          const epd_jpeg_layout_t *layout, void *buffer,
                          bool (*cancel)(void *), void *context)
{
    (void)input; (void)length; (void)layout; (void)buffer; (void)cancel; (void)context;
    abort();
}

static bool cancel_after_read(void *context)
{ return ((raster_decoder_t *)context)->read_calls >= cancel_reads; }

int main(int argc, char **argv)
{
    assert(argc == 7);
    unsigned w, h;
    raster_fit(1, UINT32_MAX, 684, 1216, &w, &h, false);
    assert(w == 1 && h == 1216);
    raster_fit(0, 1, 684, 1216, &w, &h, false);
    assert(w == 0 && h == 0);
    fail_at = strtoul(argv[5], NULL, 10);
    const char *capacity = getenv("RASTER_SRAM_BYTES");
    if (capacity) sram_capacity = strtoul(capacity, NULL, 10);
    fail_sram = getenv("RASTER_FAIL_SRAM") != NULL;
    const char *remove = getenv("RASTER_REMOVE_AFTER_READS");
    if (remove) remove_reads = strtoul(remove, NULL, 10);
    cancel_reads = strtoul(argv[6], NULL, 10);
    raster_decoder_t decoder = {0};
    active_decoder = &decoder;
    decoder.fd = -1;
    decoder.path = argv[1];
    decoder.max_width = strtoul(argv[2], NULL, 10);
    decoder.max_height = strtoul(argv[3], NULL, 10);
    decoder.context = &decoder;
    if (cancel_reads) decoder.cancel = cancel_after_read;
    bool ok = raster_io_open(&decoder) && raster_decode(&decoder) && raster_fingerprint(&decoder);
    if (ok)
    {
        FILE *output = fopen(argv[4], "wb");
        assert(output);
        size_t bytes = (size_t)decoder.width * decoder.height;
        assert(fwrite(decoder.gray, 1, bytes, output) == bytes);
        assert(fclose(output) == 0);
    }
    printf("%u %u %u %zu %zu\n", ok, decoder.width, decoder.height, calls, peak);
    raster_decoder_clear(&decoder);
    assert(live == 0 && sram_live == 0 && decoder.fast_used == 0);
    return 0;
}
