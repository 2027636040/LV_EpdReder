/**
 * @file main.c
 * @brief 应用入口（LVGL 版，纯 C）
 *
 * 入口职责：
 *  1. 初始化显示子系统（LVGL/LCD，先于蓝牙，避免内存竞争）；
 *  2. 初始化数据服务（蓝牙 PAN / 天气 / 书库 / 阅读）；
 *  3. 后续在此接入 UI 框架（页面栈 / 按键导航 / 低功耗状态机）。
 */
#include <rtthread.h>
#include <stdbool.h>

#include "littlevgl2rtt.h"
#include "lvgl.h"
#include "lvsf_font_manager.h"

#include "bt_pan.h"    /* 蓝牙 PAN 联网服务 */
#include "weather.h"   /* 天气数据服务      */
#include "bookshelf.h" /* 书库管理服务      */
#include "reader.h"    /* 阅读引擎服务      */
#include "buttons.h"   /* 按键服务（57 ADC 按键） */

#define DBG_TAG "main"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

extern void lv_ex_data_pool_init(void);

/* 启动阶段内存统计（调试用） */
static void mem_report(const char *tag)
{
    rt_uint32_t total = 0, used = 0, max_used = 0;

    rt_memory_info(&total, &used, &max_used);
    rt_kprintf("[mem] %-16s total=%u used=%u max=%u avail=%u\n",
               tag, (unsigned)total, (unsigned)used,
               (unsigned)max_used, (unsigned)(total - used));
}

/* 按键动作回调（临时：无 UI 时打印验证；UI 初始化后可用
   buttons_set_callback() 替换为投递/分发到 UI 的处理函数） */
static void on_ui_action(UIAction action)
{
    rt_kprintf("[key] %s\n", buttons_action_name(action));
}

int main(void)
{
    rt_kprintf("EPD Reader (LVGL) starting...\n");
    mem_report("boot");

    /* 显示子系统先初始化：
       1) LCD/EPIC 需要的大块内存（帧缓冲、JPEG 工作缓冲）先分配；
       2) 蓝牙协议栈后启动，避免抢占堆内存导致显示初始化失败。 */
    rt_err_t ret = littlevgl2rtt_init("lcd");
    if (ret != RT_EOK)
    {
        LOG_E("LVGL init failed: %d", ret);
        return ret;
    }
    mem_report("after lvgl");

    /* ---- 数据服务初始化（我们负责，UI 通过头文件接口访问） ---- */
    if (btpan_init("RT-EPD-Reader") == RT_EOK)
        rt_kprintf("btpan service started\n");
    if (weather_service_init() == RT_EOK)
        rt_kprintf("weather service started\n");
    if (bookshelf_service_init() == RT_EOK)
        rt_kprintf("bookshelf service started\n");
    if (reader_service_init() == RT_EOK)
        rt_kprintf("reader service started\n");

    /* 按键服务（57 ADC 按键：KEY1=UP / KEY2=SELECT / KEY3=DOWN，长按 KEY1=UPGLIDE） */
    buttons_init(on_ui_action);
    rt_kprintf("buttons service %s\n", buttons_ready() ? "started" : "not available");

    mem_report("after services");

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
