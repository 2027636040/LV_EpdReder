/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_HANG_DIAG_H
#define EPD_HANG_DIAG_H

#include <rtthread.h>

typedef enum
{
    HANG_DIAG_UI,
    HANG_DIAG_LCD,
    HANG_DIAG_CHANNELS
} hang_diag_channel_t;

#ifdef EPD_HANG_DIAG
void hang_diag_init(void);
void hang_diag_mark(hang_diag_channel_t channel, const char *stage,
                    rt_uint32_t detail, rt_bool_t active);
#else
static inline void hang_diag_init(void) {}
static inline void hang_diag_mark(hang_diag_channel_t channel, const char *stage,
                                 rt_uint32_t detail, rt_bool_t active)
{
    (void)channel;
    (void)stage;
    (void)detail;
    (void)active;
}
#endif

#endif
