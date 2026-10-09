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
 * @brief Initialize ADC sampling and the battery calculator.
 */
void battery_init(void);

/**
 * @brief Disable battery ADC sampling.
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
