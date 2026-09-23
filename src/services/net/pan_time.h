/* SPDX-License-Identifier: Apache-2.0 */
#ifndef PAN_TIME_H
#define PAN_TIME_H

#include <rtthread.h>
#include <stdbool.h>

rt_err_t pan_time_init(void);
/* Only posts a state change; DNS, NTP and RTC writes run on the time worker. */
void pan_time_set_link(bool connected);

#endif
