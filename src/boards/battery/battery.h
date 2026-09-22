/**
 * @file battery.h
 * @brief 电池服务接口（纯 C）
 */
#ifndef __BATTERY_H__
#define __BATTERY_H__

#include <rtthread.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化电池服务（使能 ADC、初始化电量计算器、启动巡检定时器）
 * @param ui_queue 电池事件投递的消息队列（MSG_BATTERY_CHECK / MSG_UPDATE_CHARGE_STATUS）
 */
void battery_init(rt_mq_t ui_queue);

/**
 * @brief 停止电池服务（停止巡检定时器）
 */
void battery_stop(void);

/**
 * @brief 读取当前电压（mV 的 1/10 形式，见实现）
 */
float battery_get_voltage(void);

/**
 * @brief 获取剩余电量百分比（0-100）
 */
int battery_get_percentage(void);

/**
 * @brief 是否正在充电
 */
bool battery_is_charging(void);

/**
 * @brief 获取低电量标志
 */
uint8_t battery_get_low_power_state(void);

/**
 * @brief 设置低电量标志
 */
void battery_set_low_power_state(uint8_t state);

#ifdef __cplusplus
}
#endif

#endif /* __BATTERY_H__ */
