#include <rtthread.h>
#include "littlevgl2rtt.h"
#include "lvgl.h"
#include "lvsf_font_manager.h"

#undef DBG_LEVEL
#undef LOG_TAG
#define DBG_LEVEL DBG_INFO
#define LOG_TAG "epd1.main"
#include "rtdbg.h"

extern "C" {
extern void lv_ex_data_pool_init(void);
}

extern "C"
{
int main()
{
    rt_kprintf("EPD Reader (LVGL V8 epd1) starting...\n");

    rt_err_t ret = littlevgl2rtt_init("lcd");
    if (ret != RT_EOK)
    {
        LOG_E("LVGL init failed: %d", ret);
        return ret;
    }

    lv_ex_data_pool_init();

    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_white(), 0);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, 0);

    lv_obj_t *label = lv_label_create(lv_scr_act());
    lv_label_set_text(label, "EPD Reader (LVGL V8 epd1)");
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_font_t *demo_font = lvsf_font_get("DroidSansFallback", 16);
    if (demo_font)
    {
        lv_obj_set_style_text_font(label, demo_font, 0);
    }
    rt_kprintf("EPD Reader LVGL demo font: %s\n", demo_font ? "DroidSansFallback" : "default");
    lv_obj_center(label);

    lv_obj_invalidate(lv_scr_act());
    rt_kprintf("EPD Reader LVGL UI ready, full redraw requested\n");

    bool first_task = true;
    while (1)
    {
        uint32_t ms = lv_task_handler();
        if (first_task)
        {
            rt_kprintf("EPD Reader LVGL first task handled, next=%u ms\n", ms);
            first_task = false;
        }
        rt_thread_mdelay(ms ? ms : 1);
    }

    return 0;
}
}
