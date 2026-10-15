/* SPDX-License-Identifier: Apache-2.0 */
#include "bsp_board.h"
#ifdef BSP_USING_RTTHREAD
#include <rtthread.h>
#endif

extern void BSP_GPIO_Set(int pin, int val, int is_porta);

void BSP_LCD_Reset(uint8_t level)
{
    /* The EPD panel has no reset pin. */
    (void)level;
}

void BSP_LCD_PowerDown(void)
{
    BSP_GPIO_Set(43, 0, 1);
    BSP_GPIO_Set(44, 0, 1);
    /* Keep WAKEUP high while the TPS completes its power-down sequence. */
    BSP_GPIO_Set(39, 0, 1);
}

void BSP_LCD_PowerUp(void)
{
    HAL_PIN_Set(PAD_PA24, LCDC1_TCON_SDCLK, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA25, LCDC1_TCON_SDLE, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA26, LCDC1_TCON_SDOE, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA27, LCDC1_TCON_SDSTL, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA28, LCDC1_TCON_SDAT0, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA29, LCDC1_TCON_SDAT1, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA30, LCDC1_TCON_SDAT2, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA31, LCDC1_TCON_SDAT3, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA32, LCDC1_TCON_SDAT4, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA33, LCDC1_TCON_SDAT5, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA37, LCDC1_TCON_SDAT6, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA38, LCDC1_TCON_SDAT7, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA40, LCDC1_TCON_GDCLK, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA41, LCDC1_TCON_GDSP, PIN_NOPULL, 1);

    HAL_PIN_Set(PAD_PA39, GPIO_A39, PIN_NOPULL, 1);
    BSP_GPIO_Set(39, 0, 1);
    HAL_PIN_Set(PAD_PA42, GPIO_A42, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA43, GPIO_A43, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA44, GPIO_A44, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA52, GPIO_A52, PIN_PULLUP, 1);
    HAL_RCC_HCPU_ClockSelect(RCC_CLK_MOD_LCDC, RCC_CLK_LCDC_SYSCLK);
}

void BSP_LCD_GMODE_Set(uint8_t level)
{
    BSP_GPIO_Set(39, level, 1);
}

void BSP_TP_Reset(uint8_t level)
{
    HAL_PIN_Set(PAD_PA04, GPIO_A4, PIN_NOPULL, 1);
    BSP_GPIO_Set(4, level, 1);
}

void BSP_TP_PowerUp(void)
{
    HAL_PIN_Set(PAD_PA05, GPIO_A5, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA03, GPIO_A3, PIN_NOPULL, 1);
    BSP_TP_Reset(0);
    HAL_Delay_us(100);
    BSP_GPIO_Set(5, 1, 1);
    HAL_Delay_us(1000);
    BSP_TP_Reset(1);
#ifdef BSP_USING_RTTHREAD
    rt_thread_mdelay(200);
#else
    HAL_Delay(200);
#endif
}

void BSP_TP_PowerDown(void)
{
    BSP_TP_Reset(0);
    HAL_Delay_us(100);
    BSP_GPIO_Set(5, 0, 1);
#ifdef BSP_USING_RTTHREAD
    rt_thread_mdelay(5);
#else
    HAL_Delay(5);
#endif
}
