/**
 * @file battery.c
 * @brief 电池服务实现（纯 C）——由 ADCBattery.cpp 改写
 *
 * 说明：
 *  - ADC 设备 "bat1"，通道 7；电量由 battery_calculator（充放电曲线表）计算
 *  - 巡检定时器 10 秒周期，回调中只投递 MSG_BATTERY_CHECK 消息，
 *    实际的 ADC 读取/UI 刷新在 UI 主循环中完成（避免定时器线程中做耗时操作）
 *  - 57 平台（SF32LB57X）电池采集方案待接入，当前提供占位实现
 */
#include "battery.h"

#define DBG_TAG "battery"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#ifndef SF32LB57X

#include <math.h>

#include "rtdevice.h"
#include "board.h"
#include "battery_calculator.h"
#include "ui_events.h"

static rt_device_t s_adc_dev = RT_NULL;
static int s_adc_channel = 7;
static rt_timer_t s_check_timer = RT_NULL;
static rt_mq_t s_ui_queue = RT_NULL;
static uint8_t s_low_power = 0;
static battery_calculator_t s_calc;

static void battery_check_callback(void *parameter)
{
    (void)parameter;

    /* 定时器回调上下文：只投递消息，让 UI 主循环执行实际工作 */
    if (s_ui_queue)
    {
        UIAction msg = MSG_BATTERY_CHECK;
        rt_mq_send(s_ui_queue, &msg, sizeof(UIAction));
    }
}

void battery_init(rt_mq_t ui_queue)
{
    s_ui_queue = ui_queue;
    s_adc_dev = rt_device_find("bat1");
    s_adc_channel = 7;
    s_low_power = 0;

    if (s_adc_dev)
    {
        rt_adc_enable((rt_adc_device_t)s_adc_dev, s_adc_channel);
    }

    /* 电池计算器配置（充放电曲线来自板级 battery_table.c） */
    static const battery_calculator_config_t config =
    {
        .charging_table = charging_curve_table,
        .charging_table_size = charging_curve_table_size,
        .discharging_table = discharge_curve_table,
        .discharging_table_size = discharge_curve_table_size,
        .charge_filter_threshold = 50,          /* 充电时电压变化滤波阈值(mV) */
        .discharge_filter_threshold = 50,       /* 放电时电压变化滤波阈值(mV) */
        .filter_count = 3,                      /* 滤波计数阈值 */
        .secondary_filter_enabled = true,       /* 启用二级滤波 */
        .secondary_filter_weight_pre = 90,      /* 上次电压权重 */
        .secondary_filter_weight_cur = 10,      /* 当前电压权重 */
    };

    int result = battery_calculator_init(&s_calc, &config);
    if (result != BATTERY_CALC_SUCCESS)
    {
        LOG_E("battery calculator init failed: %d", result);
    }

    /* 10 秒周期巡检 */
    s_check_timer = rt_timer_create("bat_check", battery_check_callback, RT_NULL,
                                    rt_tick_from_millisecond(10000),
                                    RT_TIMER_FLAG_PERIODIC | RT_TIMER_FLAG_SOFT_TIMER);
    if (s_check_timer != RT_NULL)
    {
        rt_timer_start(s_check_timer);
        LOG_I("battery monitor started");
    }
    else
    {
        LOG_E("failed to create battery check timer");
    }
}

void battery_stop(void)
{
    if (s_check_timer != RT_NULL)
    {
        rt_timer_stop(s_check_timer);
        rt_timer_delete(s_check_timer);
        s_check_timer = RT_NULL;
    }
}

float battery_get_voltage(void)
{
    if (!s_adc_dev)
    {
        LOG_W("ADC device not found");
        return 0.0f;
    }
    rt_uint32_t value = rt_adc_read((rt_adc_device_t)s_adc_dev, s_adc_channel);
    return value / 10.0f;
}

int battery_get_percentage(void)
{
    float voltage = battery_get_voltage();
    uint8_t percentage = battery_calculator_get_percent(&s_calc, (uint32_t)(voltage * 10));
    return percentage;
}

bool battery_is_charging(void)
{
    int pin_val = rt_pin_read(CHG_STATUS);
    return pin_val == 0;
}

uint8_t battery_get_low_power_state(void)
{
    return s_low_power;
}

void battery_set_low_power_state(uint8_t state)
{
    s_low_power = state;
}

#else /* SF32LB57X：电池采集方案待接入，提供占位实现 */

void battery_init(rt_mq_t ui_queue)
{
    (void)ui_queue;
}

void battery_stop(void)
{
}

float battery_get_voltage(void)
{
    return 0.0f;
}

int battery_get_percentage(void)
{
    return 0;
}

bool battery_is_charging(void)
{
    return false;
}

uint8_t battery_get_low_power_state(void)
{
    return 0;
}

void battery_set_low_power_state(uint8_t state)
{
    (void)state;
}

#endif /* !SF32LB57X */
