/**
 * @file main.c
 * @brief 应用入口（LVGL 版，纯 C）
 *
 * 当前为最小验证程序：初始化 LVGL 并显示一行文本。
 * 后续在此接入 UI 框架（页面栈 / 按键导航 / 低功耗状态机）。
 */
#include <rtthread.h>
#include <stdbool.h>

#include "littlevgl2rtt.h"
#include "lvgl.h"
#include "lvsf_font_manager.h"

#define DBG_TAG "main"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

extern void lv_ex_data_pool_init(void);

int main(void)
{
    rt_kprintf("EPD Reader (LVGL) starting...\n");

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
    lv_label_set_text(label, "EPD Reader (LVGL)");
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
