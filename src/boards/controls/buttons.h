/**
 * @file buttons.h
 * @brief 按键服务接口（纯 C，57 板 ADC 按键）
 *
 * 三个物理键（KEY1/KEY2/KEY3）共用 PA34 中断脚，经 ADC 分压区分按键，
 * 产生 UIAction 动作（UP/DOWN/SELECT/UPGLIDE）经回调分发给应用。
 *
 * 动作映射：
 *   KEY1 单击 -> UP        KEY1 长按 -> UPGLIDE
 *   KEY2 单击 -> SELECT
 *   KEY3 单击 -> DOWN
 *
 * 线程说明：动作回调运行在 button 库的软定时器线程上下文，
 *           UI 侧请用 lv_async_call() 切回 UI 线程（勿在回调里直接操作 LVGL）。
 */
#ifndef __BUTTONS_H__
#define __BUTTONS_H__

#include <stdbool.h>
#include <stdint.h>

#include "ui_events.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化按键服务（注册 button 库回调，启用 ADC 按键检测）
 * @param on_action 动作回调（可为 NULL，后续用 buttons_set_callback 接管）
 */
void buttons_init(ActionCallback_t on_action);

/** 运行期注册/替换动作回调（UI 初始化完成后调用；传 NULL 注销） */
void buttons_set_callback(ActionCallback_t on_action);

/** 动作名称字符串（打印用：UP / DOWN / SELECT / UPGLIDE） */
const char *buttons_action_name(UIAction action);

/** 按键服务是否初始化成功 */
bool buttons_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* __BUTTONS_H__ */
