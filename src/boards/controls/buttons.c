/**
 * @file buttons.c
 * @brief 按键服务实现（纯 C）——由 SF32_ButtonControls.cpp 改写
 *
 * 57 板（dpi-hdk_lb57gyd7n6_epd）按键方案：
 *   - 三个物理键共用 1 个 GPIO 作为中断/唤醒脚（BSP_KEY1_PIN），
 *     按下后经 ADC 电压采样区分具体按键（USING_ADC_BUTTON）
 *   - 单击：索引 0/1/2  -> UP / SELECT / DOWN（对应 KEY1 / KEY2 / KEY3）
 *   - 长按：索引 0      -> UPGLIDE（呼出全局操作）
 */
#include "buttons.h"

#include "board.h"
#include "button.h"

#include <rtthread.h>
#include <rtdevice.h>

#define DBG_TAG "buttons"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

static ActionCallback_t s_action_cb = RT_NULL;

#ifdef SF32LB57X

static void dummy_button_event_handler(int32_t pin, button_action_t action)
{
    /* 57 板按键全部走 ADC 按键路径，此 GPIO 回调仅作占位 */
    (void)pin;
    (void)action;
}

#ifdef USING_ADC_BUTTON
static void adc_button_handler(uint8_t group_idx, int32_t btn_idx, button_action_t button_action)
{
    (void)group_idx;

    if (s_action_cb == RT_NULL)
    {
        return;
    }

    if (button_action == BUTTON_CLICKED)
    {
        if (0 == btn_idx)
        {
            s_action_cb(UP);
        }
        else if (1 == btn_idx)
        {
            s_action_cb(SELECT);
        }
        else if (2 == btn_idx)
        {
            s_action_cb(DOWN);
        }
    }
    else if (button_action == BUTTON_LONG_PRESSED)
    {
        if (0 == btn_idx)
        {
            s_action_cb(UPGLIDE);
        }
    }
}
#endif /* USING_ADC_BUTTON */

void buttons_init(ActionCallback_t on_action)
{
    s_action_cb = on_action;

    int32_t id;
    button_cfg_t cfg;

    cfg.pin = BSP_KEY1_PIN;

#ifdef BSP_KEY1_ACTIVE_HIGH
    cfg.active_state = BUTTON_ACTIVE_HIGH;
#else
    cfg.active_state = BUTTON_ACTIVE_LOW;
#endif

    cfg.mode = PIN_MODE_INPUT;
    cfg.button_handler = dummy_button_event_handler;
    id = button_init(&cfg);
    RT_ASSERT(id >= 0);

#ifdef USING_ADC_BUTTON
    {
        adc_button_handler_t handlers[ADC_BUTTON_GROUP1_MAX_NUM];
        for (uint8_t i = 0; i < ADC_BUTTON_GROUP1_MAX_NUM; i++)
        {
            handlers[i] = adc_button_handler;
        }
        rt_err_t err = button_bind_adc_button(id, 0, ADC_BUTTON_GROUP1_MAX_NUM, handlers);
        RT_ASSERT(0 == err);
    }
#endif /* USING_ADC_BUTTON */

    if (SF_EOK != button_enable(id))
    {
        RT_ASSERT(0);
    }

    LOG_I("buttons initialized");
}

#else /* 非 57 平台暂未迁移按键方案 */

void buttons_init(ActionCallback_t on_action)
{
    (void)on_action;
}

#endif /* SF32LB57X */
