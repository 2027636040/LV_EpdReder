#include "raster.h"
#include "epd_image.h"
#include "epd_tjpgd.h"
#include <string.h>

#define JPEG_POOL_BYTES (20u * 1024u)

static size_t jpeg_input(JDEC *jpeg, uint8_t *data, size_t bytes)
{
    return raster_read_some(jpeg->device, data, bytes);
}

static int jpeg_output(JDEC *jpeg, void *data, JRECT *rectangle)
{
    raster_decoder_t *decoder = jpeg->device;
    if (raster_cancelled(decoder)) return 0;
    const uint8_t *pixel = data;
    unsigned width = rectangle->right - rectangle->left + 1;
    for (unsigned y = rectangle->top; y <= rectangle->bottom; ++y)
    {
        raster_sink_row(decoder, rectangle->left, y, 1, pixel, width, RASTER_GRAY8);
        pixel += width;
    }
    if (rectangle->right + 1u == decoder->sample_width)
        raster_sink_end_band(decoder, rectangle->bottom + 1u);
    return 1;
}

/* 0: use software, 1: complete, -1: cancelled or allocation/input failed. */
static int hardware_decode(raster_decoder_t *decoder, const JDEC *jpeg)
{
    unsigned w = jpeg->width, h = jpeg->height;
    /* Match the SF32LB57 AHB decoder's YCbCr 4:2:0 path. Other baseline
     * sampling formats remain supported by the streaming software decoder. */
    if (decoder->forward_only || jpeg->ncomp != 3 || jpeg->msx != 2 || jpeg->msy != 2) return 0;
    epd_jpeg_layout_t layout;
    if (!epd_image_jpeg_layout(w, h, &layout)) return 0;
    size_t position = decoder->position;
    size_t length = decoder->file_size;
    if (length > SIZE_MAX - 63u) return 0;
    void *output_memory = epd_app_alloc(layout.buffer_size + 63, EPD_APP_PSRAM);
    void *input_memory = output_memory ? epd_app_alloc(RT_ALIGN(length, 32) + 31, EPD_APP_PSRAM) : NULL;
    if (!input_memory)
    {
        epd_app_free(output_memory);
        rt_kprintf("[image] JPEGD RAM1 allocation failed, use streaming decode\n");
        return 0;
    }
    uint8_t *input = (uint8_t *)RT_ALIGN((uintptr_t)input_memory, 32);
    uint8_t *output = (uint8_t *)RT_ALIGN((uintptr_t)output_memory, 64);
    bool ok = raster_seek(decoder, 0) && raster_read(decoder, input, length);
    if (ok && !raster_cancelled(decoder))
        ok = epd_image_jpeg_decode(input, length, &layout, output, decoder->cancel, decoder->context);
    else ok = false;
    epd_app_free(input_memory);
    int result = 0;
    if (ok)
    {
        result = raster_sink_init(decoder, w, h, true) ? 1 : 0;
        if (result == 0)
        {
            /* Releasing the full hardware frame may let streaming decode fit. */
            raster_fast_free(decoder, decoder->sum);
            raster_fast_free(decoder, decoder->x_map);
            epd_app_free(decoder->gray);
            decoder->sum = NULL;
            decoder->x_map = NULL;
            decoder->gray = NULL;
            decoder->error[0] = '\0';
        }
        const uint8_t *gray = output + layout.work_size;
        for (unsigned y = 0; result == 1 && y < h; ++y)
        {
            if (raster_cancelled(decoder)) { result = -1; break; }
            raster_sink_row(decoder, 0, y, 1, gray + y * layout.stride, w, RASTER_GRAY8);
        }
        if (result == 1) rt_kprintf("[image] JPEGD hardware gray, RAM1 buffer=%u\n", (unsigned)layout.buffer_size);
    }
    epd_app_free(output_memory);
    raster_seek(decoder, position);
    if (decoder->io_error || raster_cancelled(decoder)) result = -1;
    return result;
}

bool raster_decode_jpeg(raster_decoder_t *decoder)
{
    JDEC *jpeg = raster_fast_alloc(decoder, sizeof(*jpeg));
    void *pool = raster_fast_alloc(decoder, JPEG_POOL_BYTES);
    if (!jpeg || !pool)
    {
        raster_fast_free(decoder, jpeg);
        raster_fast_free(decoder, pool);
        strcpy(decoder->error, "图片内存不足");
        return false;
    }
    JRESULT code = jd_prepare(jpeg, jpeg_input, pool, JPEG_POOL_BYTES, decoder);
    bool ok = code == JDR_OK;
    if (ok)
    {
        decoder->source_width = jpeg->width;
        decoder->source_height = jpeg->height;
        int hardware = hardware_decode(decoder, jpeg);
        ok = hardware >= 0;
        if (hardware == 0)
        {
            unsigned w, h, scale = 0;
            raster_fit(jpeg->width, jpeg->height, decoder->max_width, decoder->max_height, &w, &h, false);
            while (scale < 3 && (jpeg->width >> (scale + 1)) >= w && (jpeg->height >> (scale + 1)) >= h)
                ++scale;
            ok = raster_sink_init_banded(decoder, jpeg->width >> scale, jpeg->height >> scale,
                                        (jpeg->msy * 8u) >> scale);
            if (ok)
            {
                rt_kprintf("[image] JPEG gray MCU streaming, scale=1/%u\n", 1u << scale);
                code = jd_decomp(jpeg, jpeg_output, scale);
                ok = code == JDR_OK;
            }
        }
    }
    if (!ok && !decoder->error[0])
        strcpy(decoder->error, code == JDR_FMT2 || code == JDR_FMT3 ?
               "不支持此 JPEG 编码" : "JPEG 解码失败");
    raster_fast_free(decoder, jpeg);
    raster_fast_free(decoder, pool);
    return ok;
}
