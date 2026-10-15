/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_PANEL_INIT_H
#define EPD_PANEL_INIT_H

#include <rtthread.h>

#ifdef BSP_USING_BOARD_SF32LB57_EPD_GYD7N6
rt_err_t epd_panel_boot(void);
#else
static inline rt_err_t epd_panel_boot(void) { return RT_EOK; }
#endif

#endif
