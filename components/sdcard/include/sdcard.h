/*
 * Copyright (c) 2026, SiFli Technology
 * SPDX-License-Identifier: LicenseRef-SiFli
 * See LICENSE in the component root.
 */
#ifndef __SDCARD_H__
#define __SDCARD_H__

#include <rtthread.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    SDCARD_ABSENT = 0,
    SDCARD_PROBING,
    SDCARD_READY,
    SDCARD_DRAINING,
    SDCARD_UNMOUNTING,
    SDCARD_FAULT,
} sdcard_state_t;

typedef enum
{
    SDCARD_ERROR_NONE = 0,
    SDCARD_ERROR_START,
    SDCARD_ERROR_PROBE_SUBMIT,
    SDCARD_ERROR_PROBE_WAIT,
    SDCARD_ERROR_PROBE_RESULT,
    SDCARD_ERROR_MOUNT,
    SDCARD_ERROR_RELEASE_WAIT,
    SDCARD_ERROR_OPEN_FILES,
    SDCARD_ERROR_UNMOUNT,
    SDCARD_ERROR_REMOVE_SUBMIT,
    SDCARD_ERROR_REMOVE_WAIT,
    SDCARD_ERROR_REMOVE_RESULT,
} sdcard_error_stage_t;

typedef struct
{
    const char *device_name;
    const char *mount_point;
    rt_base_t detect_pin;
    rt_base_t present_level;
    rt_mutex_t io_lock;
} sdcard_config_t;

typedef struct
{
    sdcard_state_t state;
    rt_bool_t present;       /* Last debounced pin value, not readiness. */
    rt_bool_t mounted;       /* Remains true until DFS unmount succeeds. */
    rt_bool_t available;     /* Revoked immediately on a removal IRQ. */
    uint32_t session;        /* Valid only with available; zero is reserved. */
    uint32_t release_id;     /* Current retirement request; zero when none. */
    uint32_t revision;
    sdcard_error_stage_t error_stage;
    rt_err_t error;
} sdcard_snapshot_t;

/**
 * Start the single, resident SDIO card service from thread context, after SDK
 * device initialization. The configuration is copied; strings, the mutex and
 * the SDK host must remain valid for the firmware lifetime. The mount path must
 * be an existing, canonical absolute directory, not the root directory.
 *
 * The board owns pin mux/pull configuration. This service owns only the input
 * mode, detection IRQ and card lifecycle. No other service may mount/unmount or
 * submit card changes for this host. The pin must match SD_INSERT_DETECT_PIN;
 * this SDK's startup detector supports active-low card detection.
 *
 * All file users share io_lock, check available/session before new I/O, and
 * retain their original session. They may close/release invalidated objects.
 * Return RT_EOK when the worker is started, not when the card is mounted.
 * Initialization failure unwinds owned resources and permits a later retry.
 */
rt_err_t sdcard_start(const sdcard_config_t *config);

/** Copy a coherent snapshot. No internal mutable object is returned. */
void sdcard_get_snapshot(sdcard_snapshot_t *snapshot);

/**
 * Acknowledge exactly one release request after all users have stopped and
 * released files, raw device accesses and asynchronous work. Call from thread
 * context without holding io_lock. Stale IDs return -RT_EINVAL. Repeating an
 * accepted acknowledgment while still DRAINING returns -RT_EBUSY. No forced
 * release occurs.
 */
rt_err_t sdcard_release_complete(uint32_t release_id);

#ifdef __cplusplus
}
#endif
#endif
