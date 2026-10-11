/* SPDX-License-Identifier: Apache-2.0 */
#include "bsp_board.h"
#ifdef BSP_USING_RTTHREAD
#include "board_ioex.h"
#endif

extern void BSP_GPIO_Set(int pin, int val, int is_porta);

void BSP_LCD_Reset(uint8_t level)
{
    /* The direct-EPD adapter does not use LCD_RST. */
    (void)level;
}

void BSP_LCD_PowerDown(void)
{
    BSP_LCD_Reset(0);
}

void BSP_LCD_PowerUp(void)
{
    HAL_PIN_Set(PAD_PA52, GPIO_A52, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA53, GPIO_A53, PIN_NOPULL, 1);
    BSP_GPIO_Set(52, 1, 1); /* FPC B3: EPD 3.3 V enable */
    BSP_GPIO_Set(53, 1, 1); /* FPC B4: touch 3.3 V enable */

    /* Preserve the DPI-HDK adapter's connector signals, not its MCU pad numbers. */
    HAL_PIN_Set(PAD_PA50, LCDC1_TCON_GDSP, PIN_NOPULL, 1);  /* DE */
    HAL_PIN_Set(PAD_PA49, LCDC1_TCON_GDCLK, PIN_NOPULL, 1); /* HSYNC */
    HAL_PIN_Set_DS0(PAD_PA49, 1, 0);
    HAL_PIN_Set_DS1(PAD_PA49, 1, 0);
    HAL_PIN_Set(PAD_PA26, LCDC1_TCON_SDCLK, PIN_NOPULL, 1); /* R2 */
    HAL_PIN_Set(PAD_PA48, LCDC1_TCON_SDAT15, PIN_NOPULL, 1);/* CLK */
    HAL_PIN_Set(PAD_PA27, LCDC1_TCON_SDLE, PIN_NOPULL, 1);  /* R3 */
    HAL_PIN_Set(PAD_PA47, LCDC1_TCON_SDAT14, PIN_NOPULL, 1);/* B7 */
    HAL_PIN_Set(PAD_PA28, LCDC1_TCON_SDOE, PIN_NOPULL, 1);  /* R4 */
    HAL_PIN_Set(PAD_PA46, LCDC1_TCON_SDAT13, PIN_NOPULL, 1);/* B6 */
    HAL_PIN_Set(PAD_PA45, LCDC1_TCON_SDAT12, PIN_NOPULL, 1);/* B2 */
    HAL_PIN_Set(PAD_PA29, LCDC1_TCON_SDSTL, PIN_NOPULL, 1); /* R5 */
    HAL_PIN_Set(PAD_PA44, LCDC1_TCON_SDAT11, PIN_NOPULL, 1);/* B1 */
    HAL_PIN_Set(PAD_PA30, LCDC1_TCON_SDAT0, PIN_NOPULL, 1); /* R6 */
    HAL_PIN_Set(PAD_PA43, LCDC1_TCON_SDAT10, PIN_NOPULL, 1);/* B0 */
    HAL_PIN_Set(PAD_PA31, LCDC1_TCON_SDAT1, PIN_NOPULL, 1); /* R7 */
    HAL_PIN_Set(PAD_PA42, LCDC1_TCON_SDAT9, PIN_NOPULL, 1); /* G7 */
    HAL_PIN_Set(PAD_PA32, LCDC1_TCON_SDAT2, PIN_NOPULL, 1); /* G0 */
    HAL_PIN_Set(PAD_PA41, LCDC1_TCON_SDAT8, PIN_NOPULL, 1); /* G6 */
    HAL_PIN_Set(PAD_PA33, LCDC1_TCON_SDAT3, PIN_NOPULL, 1); /* G1 */
    HAL_PIN_Set(PAD_PA40, LCDC1_TCON_SDAT7, PIN_NOPULL, 1); /* G5 */
    HAL_PIN_Set(PAD_PA37, LCDC1_TCON_SDAT4, PIN_NOPULL, 1); /* G2 */
    HAL_PIN_Set(PAD_PA39, LCDC1_TCON_SDAT6, PIN_NOPULL, 1); /* G4 */
    HAL_PIN_Set(PAD_PA38, LCDC1_TCON_SDAT5, PIN_NOPULL, 1); /* G3 */

    HAL_PIN_Set(PAD_PA54, GPIO_A54, PIN_PULLUP, 1);        /* B5: GMODE */
    HAL_PIN_Set(PAD_PA51, GPIO_A51, PIN_NOPULL, 1);        /* VSYNC: TPS WAKEUP */
    HAL_PIN_Set(PAD_PA24, GPIO_A24, PIN_NOPULL, 1);        /* R0: TPS PWRCOM */
    HAL_PIN_Set(PAD_PA25, GPIO_A25, PIN_NOPULL, 1);        /* R1: TPS PWRUP */
    HAL_PIN_Set(PAD_PA11, GPTIM1_CH4, PIN_NOPULL, 1);
    HAL_RCC_HCPU_ClockSelect(RCC_CLK_MOD_LCDC, RCC_CLK_LCDC_SYSCLK);
}

void BSP_LCD_GMODE_Set(uint8_t level)
{
    BSP_GPIO_Set(54, level, 1);
}

void BSP_TP_Reset(uint8_t level)
{
#ifdef BSP_USING_RTTHREAD
    board_ioex_write(14, level); /* AW9523B P1.6 */
#else
    (void)level;
#endif
}

void BSP_TP_PowerUp(void)
{
    BSP_TP_Reset(1);
    HAL_PIN_Set(PAD_PA20, GPIO_A20, PIN_PULLUP, 1);
}

void BSP_TP_PowerDown(void)
{
    BSP_TP_Reset(0);
    HAL_PIN_Set(PAD_PA20, GPIO_A20, PIN_NOPULL, 1);
}

uint32_t BSP_GET_DPI_AUX_VSYNC_PIN(void)
{
    return 51;
}
