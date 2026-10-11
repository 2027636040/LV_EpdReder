/* SPDX-License-Identifier: Apache-2.0 */
#include "bsp_board.h"

void BSP_PIN_Init(void)
{
#ifdef SOC_BF0_HCPU
    HAL_PIN_Set_Analog(PAD_PA55, 1);
    HAL_PIN_Set_Analog(PAD_PA56, 1);
#ifdef BSP_USING_SDMMC1
    HAL_PIN_Set(PAD_PA08, SD1_CLK, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA09, SD1_CMD, PIN_PULLUP, 1);
    HAL_PIN_Set(PAD_PA10, SD1_DIO0, PIN_PULLUP, 1);
#endif
    HAL_PIN_Set(PAD_PA22, I2C1_SCL, PIN_PULLUP, 1);
    HAL_PIN_Set(PAD_PA23, I2C1_SDA, PIN_PULLUP, 1);
    HAL_PIN_Set(PAD_PA20, GPIO_A20, PIN_PULLUP, 1);
    HAL_PIN_Set(PAD_PA21, GPIO_A21, PIN_PULLUP, 1);
#endif
}
