/**
 * @file main.c
 * @brief 应用入口（LVGL 版，纯 C）
 *
 * 入口职责：
 *  1. 初始化显示子系统（LVGL/LCD，先于蓝牙，避免内存竞争）；
 *  2. 初始化数据服务（蓝牙 PAN / 天气 / 书库 / 阅读）；
 *  3. 临时验证页：按键 -> 屏幕刷新（EPD 全刷/局刷周期验证）；UI 同事接手后替换为正式页面。
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
#include "epd_waveform.h" /* EPD 波形（验证页：请求全刷） */
#include "board_service.h" /* 板级服务（TF 卡文件系统挂载） */

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

/* ---- 临时验证页：按键 -> 屏幕刷新（刷新周期验证 + 标准对接模式演示） ---- */
static lv_obj_t *s_count_label = NULL;
static lv_obj_t *s_block = NULL;
static int s_count = 0;
static int s_block_pos = 0;

/* 残影观察块位置（四角轮转） */
static const int s_block_pos_tab[4][2] =
{
    { 40, 420 }, { 444, 420 }, { 444, 900 }, { 40, 900 }
};

/* UI 线程执行体（由 lv_async_call 调度） */
static void verify_page_update(void *arg)
{
    UIAction action = (UIAction)(rt_ubase_t)arg;

    if (s_count_label == NULL || s_block == NULL)
    {
        return;   /* 页面尚未就绪（理论上不会走到） */
    }

    switch (action)
    {
    case UP:
        s_block_pos = (s_block_pos + 3) & 3;   /* 上一个位置 */
        break;

    case DOWN:
        s_block_pos = (s_block_pos + 1) & 3;   /* 下一个位置 */
        break;

    case SELECT:
        s_count++;
        lv_label_set_text_fmt(s_count_label, "count: %d", s_count);
        break;

    case UPGLIDE:
        /* 长按 KEY1：请求下一次刷新强制全刷（清残影验证） */
        epd_wave_request_full();
        s_count = 0;
        lv_label_set_text(s_count_label, "count: 0 (FULL requested)");
        break;

    default:
        break;
    }

    lv_obj_set_pos(s_block, s_block_pos_tab[s_block_pos][0], s_block_pos_tab[s_block_pos][1]);
}

/* 按键动作回调（button 库线程上下文 -> lv_async_call 切回 UI 线程） */
static void on_ui_action(UIAction action)
{
    rt_kprintf("[key] %s\n", buttons_action_name(action));
    lv_async_call(verify_page_update, (void *)(rt_ubase_t)action);
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

    /* 挂载 TF 卡文件系统（挂载点 "/"，无卡时回退内置 flash 分区）；
       书库/阅读服务以该根目录为工作目录，书籍直接放卡根目录即可 */
    board_start_filesystem();
    mem_report("after mount");

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

    /* ---- 临时验证页（UI 同事接手后整体替换） ---- */
    lv_font_t *demo_font = lvsf_font_get("DroidSansFallback", 16);

    lv_obj_t *title = lv_label_create(lv_scr_act());
    lv_label_set_text(title, "EPD Refresh Test");
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    if (demo_font)
    {
        lv_obj_set_style_text_font(title, demo_font, 0);
    }
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    s_count_label = lv_label_create(lv_scr_act());
    lv_label_set_text(s_count_label, "count: 0");
    lv_obj_set_style_text_color(s_count_label, lv_color_black(), 0);
    if (demo_font)
    {
        lv_obj_set_style_text_font(s_count_label, demo_font, 0);
    }
    lv_obj_align(s_count_label, LV_ALIGN_TOP_MID, 0, 60);

    lv_obj_t *hint = lv_label_create(lv_scr_act());
    lv_label_set_text(hint,
                      "KEY1/KEY3: move block\n"
                      "KEY2: count+1\n"
                      "long KEY1: request FULL");
    lv_obj_set_style_text_color(hint, lv_color_black(), 0);
    if (demo_font)
    {
        lv_obj_set_style_text_font(hint, demo_font, 0);
    }
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 100);

    /* 残影观察块（黑色，四角轮转） */
    s_block = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_block, 200, 140);
    lv_obj_set_style_bg_color(s_block, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_block, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_block, 0, 0);
    lv_obj_set_style_radius(s_block, 0, 0);
    lv_obj_set_pos(s_block, s_block_pos_tab[0][0], s_block_pos_tab[0][1]);

    rt_kprintf("EPD Reader LVGL test page ready\n");

    lv_obj_invalidate(lv_scr_act());

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
