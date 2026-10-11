#ifndef LCD_DEVKIT_BOARD_IOEX_H
#define LCD_DEVKIT_BOARD_IOEX_H

#include <rtthread.h>

/* P0.1 is exposed through RT-Thread PIN for the existing SD card service. */
#define BOARD_IOEX_SD_PIN 129

rt_err_t board_ioex_write(unsigned pin, int value);

#endif
