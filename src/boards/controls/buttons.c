/**
 * @file buttons.c
 * @brief 按键服务实现（纯 C，57 板 ADC 按键）
 *
 * 57 板（dpi-hdk_lb57gyd7n6_epd）按键方案：
 *   - 三个物理键（KEY1/KEY2/KEY3）共用 1 个 GPIO（BSP_KEY1_PIN=PA34）作为中断脚，
 *     按下后经 ADC（"bat1" 设备、通道 6）采样电压区分具体按键（USING_ADC_BUTTON，
 *     电压表见板卡配置 CONFIG_ADC_BUTTON_GROUP1_BUTTONx_VOLT）；
 *   - 单击：索引 0/1/2 -> UP / SELECT / DOWN（对应 KEY1 / KEY2 / KEY3）；
 *   - 长按：索引 0    -> UPGLIDE（呼出全局操作）；
 *   - 回调运行在 button 库软定时器线程上下文，UI 侧请 lv_async_call 切回 UI 线程。
 *
 * 自测（无 UI 时）：直接按物理键，main 的临时回调会打印 [key] UP/SELECT/DOWN/UPGLIDE。
 */
#include "buttons.h"

#include "board.h"
#include "button.h"

#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>

#define DBG_TAG "buttons"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

static ActionCallback_t s_action_cb = RT_NULL;

static volatile bool s_ready = false;

const char *buttons_action_name(UIAction action)
{
    switch (action)
    {
    case NONE:        return "NONE";
    case UP:          return "UP";
    case DOWN:        return "DOWN";
    case SELECT:      return "SELECT";
    case UPGLIDE:     return "UPGLIDE";
    case PREV_OPTION: return "PREV_OPTION";
    case NEXT_OPTION: return "NEXT_OPTION";
    case SELECT_BOX:  return "SELECT_BOX";
    case ENTER_READING_SETTINGS: return "ENTER_READING_SETTINGS";
    case LAST_INTERACTION:       return "LAST_INTERACTION";
    default:          return "?";
    }
}

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
    UIAction action = NONE;

    (void)group_idx;

    switch (button_action)
    {
    case BUTTON_CLICKED:
        if (0 == btn_idx)
        {
            action = UP;
        }
        else if (1 == btn_idx)
        {
            action = SELECT;
        }
        else if (2 == btn_idx)
        {
            action = DOWN;
        }
        break;

    case BUTTON_LONG_PRESSED:
        if (0 == btn_idx)
        {
            action = UPGLIDE;
        }
        break;

    default:
        break;
    }

    if (NONE != action && s_action_cb != RT_NULL)
    {
        s_action_cb(action);
    }
}
#endif /* USING_ADC_BUTTON */

void buttons_init(ActionCallback_t on_action)
{
    s_action_cb = on_action;
    s_ready = false;

#ifdef USING_ADC_BUTTON
    int32_t id;
    button_cfg_t cfg;

    /* 前置防呆：ADC 设备不存在时安全退出（button 库绑定失败会 assert 死机） */
    if (RT_NULL == rt_device_find(ADC_BUTTON_ADC_DEV_NAME))
    {
        LOG_E("adc device \"%s\" not found, buttons disabled", ADC_BUTTON_ADC_DEV_NAME);
        return;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.pin = BSP_KEY1_PIN;

#ifdef BSP_KEY1_ACTIVE_HIGH
    cfg.active_state = BUTTON_ACTIVE_HIGH;
#else
    cfg.active_state = BUTTON_ACTIVE_LOW;
#endif

    cfg.mode = PIN_MODE_INPUT;
    cfg.button_handler = dummy_button_event_handler;

    id = button_init(&cfg);
    if (id < 0)
    {
        LOG_E("button_init failed: %d", (int)id);
        return;
    }

    {
        adc_button_handler_t handlers[ADC_BUTTON_GROUP1_MAX_NUM];
        uint8_t i;
        sf_err_t err;

        for (i = 0; i < ADC_BUTTON_GROUP1_MAX_NUM; i++)
        {
            handlers[i] = adc_button_handler;
        }

        err = button_bind_adc_button(id, 0, ADC_BUTTON_GROUP1_MAX_NUM, handlers);
        if (SF_EOK != err)
        {
            LOG_E("button_bind_adc_button failed: %d", (int)err);
            return;
        }
    }

    if (SF_EOK != button_enable(id))
    {
        LOG_E("button_enable failed");
        return;
    }

    s_ready = true;
    LOG_I("buttons ready: idx0=UP idx1=SELECT idx2=DOWN, long(idx0)=UPGLIDE");
#else
    LOG_W("USING_ADC_BUTTON not enabled, buttons disabled");
#endif /* USING_ADC_BUTTON */
}

#else /* 非 57 平台暂未迁移按键方案 */

void buttons_init(ActionCallback_t on_action)
{
    (void)on_action;
}

#endif /* SF32LB57X */

/*---------------------------------------------------------------------------*/
/* 公共接口（与平台无关） */
/*---------------------------------------------------------------------------*/

void buttons_set_callback(ActionCallback_t on_action)
{
    s_action_cb = on_action;
}

bool buttons_ready(void)
{
    return s_ready;
}
