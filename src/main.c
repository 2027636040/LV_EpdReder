/**
 * @file main.c
 * @brief LVGL v9 Launcher, data services and display event loop.
 */
#include <rtthread.h>
#include "bf0_hal.h"
#include "littlevgl2rtt.h"
#include "lvgl.h"
#include "src/draw/lv_draw_buf_private.h"
#include "ui/ui_app.h"
#include "bt_pan.h"
#include "weather.h"
#include "bookshelf.h"
#include "services/reader/reader.h"
#include "buttons.h"

#include "board_service.h" /* 板级服务（TF 卡文件系统挂载） */

#if LVGL_VERSION_MAJOR != 9
#error "This application requires LVGL v9."
#endif

#define DBG_TAG "main"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#if LV_USE_TINY_TTF && defined(PSRAM_CACHE_WB)
static void font_draw_buf_flush_cache(const lv_draw_buf_t *draw_buf, const lv_area_t *area)
{
    LV_UNUSED(area);
    /* Write back the whole glyph, including row padding, before EPIC reads it. */
    mpu_dcache_clean(draw_buf->data, (uint32_t)draw_buf->header.stride * draw_buf->header.h);
}
#endif

static void mem_report(const char *tag)
{
    rt_uint32_t total = 0, used = 0, max_used = 0;

    rt_memory_info(&total, &used, &max_used);
    rt_kprintf("[mem] %-16s total=%u used=%u max=%u avail=%u\n",
               tag, (unsigned)total, (unsigned)used,
               (unsigned)max_used, (unsigned)(total - used));
}

int main(void)
{
    LOG_I("LVGL %d.%d.%d starting", LVGL_VERSION_MAJOR,
          LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    mem_report("boot");

    /* Allocate display buffers before starting the Bluetooth services. */
    rt_err_t ret = littlevgl2rtt_init("lcd");
    if (ret != RT_EOK)
    {
        LOG_E("LVGL init failed: %d", ret);
        return ret;
    }

#if LV_USE_TINY_TTF && defined(PSRAM_CACHE_WB)
    lv_draw_buf_get_font_handlers()->flush_cache_cb = font_draw_buf_flush_cache;
#endif
    LOG_I("LVGL display initialized: %d x %d", LCD_HOR_RES_MAX, LCD_VER_RES_MAX);
    mem_report("after lvgl");

    /* 挂载 TF 卡文件系统（挂载点 "/"，无卡时回退内置 flash 分区）；
       书库/阅读服务以该根目录为工作目录，书籍直接放卡根目录即可 */
    board_start_filesystem();
    mem_report("after mount");

    ret = btpan_init("RT-EPD-Reader");
    if (ret != RT_EOK) LOG_W("btpan init failed: %d", ret);
    ret = weather_service_init();
    if (ret != RT_EOK) LOG_W("weather init failed: %d", ret);
    ret = bookshelf_service_init();
    if (ret != RT_EOK) LOG_W("bookshelf init failed: %d", ret);
    ret = reader_service_init();
    if (ret != RT_EOK) LOG_W("reader init failed: %d", ret);
    mem_report("after services");

    ret = ui_app_init();
    if (ret != RT_EOK)
    {
        LOG_E("Launcher init failed: %d", ret);
        return ret;
    }
    LOG_I("Buttons %s", buttons_ready() ? "ready" : "not available");
    LOG_I("Launcher ready");
    mem_report("after launcher");

    while (1)
    {
        ui_app_process();
        uint32_t ms = lv_timer_handler();
        if (ms == LV_NO_TIMER_READY || ms > 20)
        {
            ms = 20;
        }
        rt_thread_mdelay(ms ? ms : 1);
    }

    return 0;
}
