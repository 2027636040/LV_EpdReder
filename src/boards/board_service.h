/**
 * @file board_service.h
 * @brief 板级服务（纯 C）：文件系统挂载、SD 卡电源、开关机流程
 *
 * 由原 Board/SF32Paper C++ 类抽取而来，去掉多板卡抽象层。
 */
#ifndef __BOARD_SERVICE_H__
#define __BOARD_SERVICE_H__

#include <rtthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 开机流程：处理唤醒来源（冷启动/休眠唤醒/误触关机判断）
 */
void board_power_up(void);

/**
 * @brief 挂载文件系统（TF 卡优先，其次内置 flash），挂载点为 "/"
 */
void board_start_filesystem(void);

/**
 * @brief 让文件系统进入低功耗（SD 卡下电）
 */
void board_sleep_filesystem(void);

/**
 * @brief 唤醒文件系统（SD 卡上电 + 重新初始化）
 */
void board_wakeup_filesystem(void);

/**
 * @brief 进入关机/深睡前的板级处理：
 *        SD 卡下电、触摸屏下电（防止 GT967 中断误唤醒）、关闭 GPIO1 唤醒源、整机断电
 */
void board_prepare_to_sleep(void);

#ifdef __cplusplus
}
#endif

#endif /* __BOARD_SERVICE_H__ */
