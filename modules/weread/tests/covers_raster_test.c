#define COVERS_REAL_RASTER
#include "covers_test.c"
#include "epd_image.h"

static bool cancel_on_tick;
void storage_lock(void) {}
void storage_unlock(void) {}
bool storage_path_available(const char *path) { (void)path; return false; }
void rt_memory_info(rt_uint32_t *total, rt_uint32_t *used, rt_uint32_t *peak)
{ *total = 256 * 1024; *used = *peak = 0; }
rt_tick_t rt_tick_get(void)
{
    static rt_tick_t tick;
    if (cancel_on_tick) { cancel_on_tick = false; ui_cancel(); }
    return ++tick;
}
int rt_kprintf(const char *format, ...) { (void)format; return 0; }
bool epd_image_jpeg_layout(unsigned width, unsigned height, epd_jpeg_layout_t *layout)
{ (void)width; (void)height; (void)layout; return false; }
bool epd_image_jpeg_decode(const uint8_t *input, size_t length, const epd_jpeg_layout_t *layout,
    void *buffer, bool (*cancel)(void *), void *context)
{ (void)input; (void)length; (void)layout; (void)buffer; (void)cancel; (void)context; abort(); }

static uint8_t *file_bytes(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb"); assert(file);
    assert(!fseek(file, 0, SEEK_END)); long length = ftell(file); assert(length > 0);
    *size = (size_t)length; rewind(file);
    uint8_t *data = malloc(*size); assert(data && fread(data, 1, *size, file) == *size);
    assert(!fclose(file)); return data;
}
static void check_pixels(const weread_cover_t *cover, const uint8_t *expected, unsigned w, unsigned h)
{
    static const uint16_t palette[] = {0x0000,0x1082,0x2104,0x3186,0x4228,0x52aa,0x632c,0x73ae,
        0x8c51,0x9cd3,0xad55,0xbdd7,0xce79,0xdefb,0xef7d,0xffff};
    assert(cover && cover->image.header.w == w && cover->image.header.h == h && w <= 56 && h <= 72);
    assert(!((uintptr_t)cover->image.data & 31) && !(cover->image.header.stride & 3));
    for (unsigned y = 0; y < h; ++y)
    {
        const uint16_t *row = (const uint16_t *)(cover->image.data + y * cover->image.header.stride);
        for (unsigned x = 0; x < w; ++x)
        {
            unsigned found = 0;
            for (unsigned i = 0; i < 16; ++i) found += row[x] == palette[i];
            assert(found == 1);
            if (expected) assert(row[x] == palette[(expected[y * w + x] + 8) / 17]);
        }
    }
}
int main(int argc, char **argv)
{
    assert(argc == 5);
    size_t bytes, expected_size = 0;
    uint8_t *input = file_bytes(argv[1], &bytes);
    uint8_t *expected = strcmp(argv[2], "-") ? file_bytes(argv[2], &expected_size) : NULL;
    unsigned width = (unsigned)strtoul(argv[3], NULL, 10), height = (unsigned)strtoul(argv[4], NULL, 10);
    assert(!expected || expected_size == width * height);
    http_body = input; http_size = bytes;
    reset(); assert(weread_covers_request(&snapshot, 0, 1) && step());
    weread_cover_t *cover = weread_covers_take(); check_pixels(cover, expected, width, height);
    weread_covers_free(cover); assert(!live);
    reset(); assert(weread_covers_request(&snapshot, 0, 1)); cancel_on_tick = true;
    assert(step() && !weread_covers_take() && !live && !cancel_on_tick);
    reset(); http_size = 8; assert(weread_covers_request(&snapshot, 0, 1) && step());
    assert(!weread_covers_take() && !live); http_size = bytes;
    for (long fail = 0; fail < 80; ++fail)
    {
        reset(); fail_after = fail; (void)weread_covers_request(&snapshot, 0, 1);
        (void)step(); cover = weread_covers_take();
        if (cover) check_pixels(cover, expected, width, height);
        weread_covers_free(cover); weread_covers_shutdown(); fail_after = -1; assert(!live);
    }
    free(input); free(expected);
    printf("PASS: real raster %ux%u, 16-gray RGB565, cancellation/truncation/80 OOM positions, %zu allocations, no leaks.\n",
        width, height, allocations);
    return 0;
}
