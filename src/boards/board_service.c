/**
 * @file board_service.c
 * @brief 板级服务实现（纯 C）——从 SF32Paper.cpp 抽取
 *
 * 覆盖：开机唤醒处理、文件系统挂载、SD 卡上下电、关机前处理。
 * 排除了原 C++ 版本中的渲染器/字体/UI 装配逻辑（LVGL 路线由 SDK lv_lcd 接管）。
 */
#include "board_service.h"
#include "board_sdcard.h"
#include "storage.h"
#include "app_service.h"

#include <string.h>

#include "board.h"
#include "dfs_fs.h"
#include "mem_map.h"
#include "rtdevice.h"
#include "bf0_hal_aon.h"
#include "bf0_pm.h"

#if defined(RT_USING_SPI_MSD)
#include "spi_msd.h"
#endif

#ifndef _WIN32
#include "drv_flash.h"
#endif

#include "battery/battery.h"

#define DBG_TAG "board"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/* 板级/SDK 提供的电源与触摸控制接口 */
extern void BSP_TP_PowerUp(void);
extern void BSP_TP_PowerDown(void);
extern void SD_card_power_off(void);
extern void SD_card_power_on(void);
extern void PowerDownCustom(void);

void board_power_up(void)
{
    switch (SystemPowerOnModeGet())
    {
    case PM_REBOOT_BOOT:
    case PM_COLD_BOOT:
    {
        /* 正常开机 */
        break;
    }
    case PM_HIBERNATE_BOOT:
    case PM_SHUTDOWN_BOOT:
    {
        if (PMUC_WSR_RTC & pm_get_wakeup_src())
        {
            /* RTC 唤醒 */
            NVIC_EnableIRQ(RTC_IRQn);
        }
#if defined(BSP_USING_CHARGER) && !defined(SF32LB57X)
        else if ((PMUC_WSR_PIN0 << (pm_get_charger_pin_wakeup())) & pm_get_wakeup_src())
        {
            /* 充电插入唤醒：正常开机 */
        }
#endif
        else if (PMUC_WSR_PIN_ALL & pm_get_wakeup_src())
        {
#ifndef SF32LB57X
            rt_thread_mdelay(1000); /* 延时 1 秒确认按键是否长按 */
            int val = rt_pin_read(BSP_KEY1_PIN);
            rt_kprintf("Power key level after 1s: %d\n", val);
            if (val != 1)
            {
                /* 按键已松开，认为是误触发，直接关机 */
                rt_kprintf("Not long press, shutdown now.\n");
                PowerDownCustom();
                while (1) {};
            }
            else
            {
                rt_kprintf("Long press detected, power on as normal.\n");
            }
#else
            /* 57 平台：按键/充电等引脚唤醒统一正常开机 */
            rt_kprintf("Pin wake detected, power on as normal.\n");
#endif
        }
        else if (0 == pm_get_wakeup_src())
        {
            RT_ASSERT(0);
        }
        break;
    }
    default:
    {
        RT_ASSERT(0);
    }
    }
}

void board_start_filesystem(void)
{
    /* LCD initialization can repurpose PA11 as a backlight pin. */
    board_sdcard_prepare();
    storage_init();
}

void board_sleep_filesystem(void)
{
#ifdef RT_USING_SPI_MSD
    SD_card_power_off();
#endif
}

void board_wakeup_filesystem(void)
{
#ifdef RT_USING_SPI_MSD
    SD_card_power_on();
    int card_state = rt_pin_read(27); /* card detect pin */
    if (card_state == 0)
    {
        rt_kprintf("SD card inserted\n");
        msd_reinit();
    }
    else
    {
        rt_kprintf("SD card removed\n");
    }
#endif
}

void board_prepare_to_sleep(void)
{
    if (!app_service_flush())
    {
        LOG_W("power-off deferred: application data could not be saved");
        return;
    }
#ifdef RT_USING_SPI_MSD
    SD_card_power_off();
#endif

    /* 停止电池巡检 */
    battery_stop();

    /* 关闭触摸屏，防止 GT967 INT 引脚不停产生中断唤醒系统 */
    BSP_TP_PowerDown();
    /* 禁用 GPIO1 AON 唤醒源（触摸 IRQ 在 GPIO1 端口上） */
    HAL_HPAON_DisableWakeupSrc(HPAON_WAKEUP_SRC_GPIO1);

#ifndef SF32LB57X
    PowerDownCustom();
#endif
}
