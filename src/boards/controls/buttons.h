/**
 * @file buttons.h
 * @brief 按键服务接口（纯 C）
 */
#ifndef __BUTTONS_H__
#define __BUTTONS_H__

#include "ui_events.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化按键（注册 button 库回调），按下/长按产生 UIAction 经 on_action 分发
 * @param on_action 动作回调（一般为投递到 UI 消息队列的函数）
 */
void buttons_init(ActionCallback_t on_action);

#ifdef __cplusplus
}
#endif

#endif /* __BUTTONS_H__ */
