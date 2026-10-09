#include "board_sdcard.h"
#include "board.h"
#include <rtdevice.h>

void board_sdcard_prepare(void)
{
#ifdef PKG_USING_EPD_SDCARD
    HAL_PIN_Set(PAD_PA00 + SD_INSERT_DETECT_PIN, GPIO_A0 + SD_INSERT_DETECT_PIN, PIN_PULLUP, 1);
    rt_pin_mode(SD_INSERT_DETECT_PIN, PIN_MODE_INPUT_PULLUP);
#endif
}

#ifdef PKG_USING_EPD_SDCARD
/* The SDK samples card detect during device initialization, before main(). */
static int board_sdcard_early_init(void)
{
    board_sdcard_prepare();
    return RT_EOK;
}
INIT_PREV_EXPORT(board_sdcard_early_init);
#endif
