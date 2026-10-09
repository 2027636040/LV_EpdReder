/* SPDX-License-Identifier: LicenseRef-SiFli */
#ifndef __SDCARD_PORT_H__
#define __SDCARD_PORT_H__

#include <rtthread.h>

struct rt_mmcsd_host;

typedef enum
{
    SDCARD_CHANGE_NONE,
    SDCARD_CHANGE_PENDING,
    SDCARD_CHANGE_DONE,
} sdcard_port_change_t;

typedef struct
{
    sdcard_port_change_t change;
    rt_bool_t has_card;
    rt_bool_t sd_memory;
    rt_bool_t block_device;
} sdcard_port_status_t;

rt_err_t sdcard_port_bind(const char *device_name, struct rt_mmcsd_host **host);
void sdcard_port_get_status(struct rt_mmcsd_host *host, sdcard_port_status_t *status);
rt_err_t sdcard_port_request(struct rt_mmcsd_host *host);

#endif
