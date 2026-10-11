#include "raster.h"
#include <png.h>
#include <string.h>
#include <stdio.h>

typedef struct
{
    raster_decoder_t *decoder;
    png_structp png;
    png_infop info;
    uint8_t *row;
    void *window_reserve;
    bool window_sram;
    bool window_used;
    rt_tick_t codec_ticks, scale_ticks;
    rt_tick_t read_start, crc_start;
} png_context_t;

static png_voidp png_alloc(png_structp png, png_alloc_size_t bytes)
{
    png_context_t *context = png_get_mem_ptr(png);
    void *memory;
    if (bytes == 32768 && context->window_reserve)
    {
        memory = context->window_reserve;
        context->window_reserve = NULL;
        context->window_used = true;
    }
    else memory = raster_fast_alloc(context->decoder, bytes);
    if (!memory) strcpy(context->decoder->error, "图片内存不足");
    return memory;
}

static void png_free_memory(png_structp png, png_voidp pointer)
{
    if (!pointer) return;
    png_context_t *context = png_get_mem_ptr(png);
    raster_fast_free(context->decoder, pointer);
}

static void png_failure(png_structp png, png_const_charp text)
{
    png_context_t *context = png_get_error_ptr(png);
    if (!context->decoder->error[0]) strcpy(context->decoder->error, "PNG 解码失败");
    rt_kprintf("[image] PNG: %s\n", text);
    png_longjmp(png, 1);
}

static void png_warning_ignore(png_structp png, png_const_charp text)
{ (void)png; (void)text; }

static void png_input(png_structp png, png_bytep data, png_size_t bytes)
{
    png_context_t *context = png_get_io_ptr(png);
    if (!raster_read(context->decoder, data, bytes)) png_error(png, "read cancelled or truncated");
}

bool raster_decode_png(raster_decoder_t *decoder)
{
    png_context_t *context = epd_app_alloc(sizeof(*context), EPD_APP_PSRAM);
    if (!context) { strcpy(decoder->error, "图片内存不足"); return false; }
    memset(context, 0, sizeof(*context));
    context->decoder = decoder;
    bool ok = false;
    /* The repeatedly accessed inflate window gets the first real RAM0 attempt. */
    context->window_reserve = raster_fast_alloc_priority(decoder, 32768);
    if (!context->window_reserve) { strcpy(decoder->error, "图片内存不足"); goto done; }
    context->window_sram = raster_fast_is_sram(context->window_reserve);
    context->png = png_create_read_struct_2(PNG_LIBPNG_VER_STRING, context, png_failure,
                                           png_warning_ignore, context, png_alloc, png_free_memory);
    if (!context->png) goto done;
    if (setjmp(png_jmpbuf(context->png))) goto done;
    context->info = png_create_info_struct(context->png);
    if (!context->info) goto done;
    png_set_read_fn(context->png, context, png_input);
    context->read_start = decoder->read_ticks;
    context->crc_start = decoder->source_crc_ticks;
    rt_tick_t tick = rt_tick_get();
    png_read_info(context->png, context->info);
    context->codec_ticks += rt_tick_get() - tick;
    unsigned width = png_get_image_width(context->png, context->info);
    unsigned height = png_get_image_height(context->png, context->info);
    int color = png_get_color_type(context->png, context->info);
    int depth = png_get_bit_depth(context->png, context->info);
    bool interlaced = png_get_interlace_type(context->png, context->info) == PNG_INTERLACE_ADAM7;
    if (!raster_sink_init(decoder, width, height, !interlaced)) goto done;
    if (depth == 16) png_set_strip_16(context->png);
    if (color == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(context->png);
    bool transparent = png_get_valid(context->png, context->info, PNG_INFO_tRNS);
    raster_pixel_format_t format;
    if (color == PNG_COLOR_TYPE_PALETTE)
    {
        png_colorp palette;
        png_bytep alpha = NULL;
        int colors = 0, alphas = 0;
        if (!png_get_PLTE(context->png, context->info, &palette, &colors))
            png_error(context->png, "missing palette");
        if (transparent) png_get_tRNS(context->png, context->info, &alpha, &alphas, NULL);
        memset(decoder->palette, 255, sizeof(decoder->palette));
        for (int i = 0; i < colors; ++i)
        {
            unsigned value = (77u * palette[i].red + 150u * palette[i].green + 29u * palette[i].blue + 128) >> 8;
            unsigned a = i < alphas ? alpha[i] : 255;
            decoder->palette[i] = (value * a + 255 * (255 - a) + 127) / 255;
        }
        if (depth < 8) png_set_packing(context->png);
        format = RASTER_INDEX8;
    }
    else
    {
        if (transparent) png_set_tRNS_to_alpha(context->png);
        bool has_alpha = (color & PNG_COLOR_MASK_ALPHA) || transparent;
        bool gray = color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA;
        format = gray ? (has_alpha ? RASTER_GRAY_ALPHA8 : RASTER_GRAY8) :
                       (has_alpha ? RASTER_RGBA8888 : RASTER_RGB888);
    }
    /* Without interlace expansion each pass supplies only its own source pixels. */
    tick = rt_tick_get();
    png_read_update_info(context->png, context->info);
    context->codec_ticks += rt_tick_get() - tick;
    static const unsigned channels[] = {1, 2, 1, 3, 4, 2};
    if (width > SIZE_MAX / channels[format]) png_error(context->png, "row size overflow");
    size_t row_bytes = (size_t)width * channels[format];
    if (png_get_rowbytes(context->png, context->info) != row_bytes ||
        png_get_channels(context->png, context->info) != channels[format])
        png_error(context->png, "unexpected transformed row format");
    context->row = raster_fast_alloc(decoder, row_bytes);
    if (!context->row) { strcpy(decoder->error, "图片内存不足"); goto done; }
    rt_kprintf("[image] PNG channels=%u, %s\n", channels[format], interlaced ? "Adam7" : "row streaming");
    static const unsigned x0[7] = {0, 4, 0, 2, 0, 1, 0}, y0[7] = {0, 0, 4, 0, 2, 0, 1};
    static const unsigned dx[7] = {8, 8, 4, 4, 2, 2, 1}, dy[7] = {8, 8, 8, 4, 4, 2, 2};
    for (unsigned pass = 0; pass < (interlaced ? 7u : 1u); ++pass)
    {
        unsigned start_x = interlaced ? x0[pass] : 0, start_y = interlaced ? y0[pass] : 0;
        unsigned step_x = interlaced ? dx[pass] : 1, step_y = interlaced ? dy[pass] : 1;
        if (start_x >= width || start_y >= height) continue;
        for (unsigned y = start_y; y < height; y += step_y)
        {
            if (raster_cancelled(decoder)) goto done;
            tick = rt_tick_get();
            png_read_row(context->png, context->row, NULL);
            context->codec_ticks += rt_tick_get() - tick;
            tick = rt_tick_get();
            raster_sink_row(decoder, start_x, y, step_x, context->row,
                             (width - start_x + step_x - 1) / step_x, format);
            context->scale_ticks += rt_tick_get() - tick;
        }
    }
    tick = rt_tick_get();
    png_read_end(context->png, NULL);
    context->codec_ticks += rt_tick_get() - tick;
    rt_tick_t read_ticks = decoder->read_ticks - context->read_start;
    rt_tick_t crc_ticks = decoder->source_crc_ticks - context->crc_start;
    rt_kprintf("[image] PNG stages: codec=%u ms, scale=%u ms, read=%u ms, source_crc=%u ms, window=%s\n",
        (unsigned)((uint64_t)(context->codec_ticks - read_ticks - crc_ticks) * 1000 / RT_TICK_PER_SECOND),
        (unsigned)((uint64_t)context->scale_ticks * 1000 / RT_TICK_PER_SECOND),
        (unsigned)((uint64_t)read_ticks * 1000 / RT_TICK_PER_SECOND),
        (unsigned)((uint64_t)crc_ticks * 1000 / RT_TICK_PER_SECOND),
        context->window_used ? (context->window_sram ? "RAM0" : "RAM1") : "unused");
    ok = true;
done:
    raster_fast_free(decoder, context->row);
    if (context->png) png_destroy_read_struct(&context->png, &context->info, NULL);
    raster_fast_free(decoder, context->window_reserve);
    epd_app_free(context);
    return ok;
}
