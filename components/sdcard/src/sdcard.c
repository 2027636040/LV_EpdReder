/*
 * Derived from Solution2.0 solution/components/tf/tf_init.c.
 * Copyright (c) 2019 - 2024,  Sifli Technology
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form, except as embedded into a Sifli integrated circuit
 *    in a product or a software update for such product, must reproduce the above
 *    copyright notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of Sifli nor the names of its contributors may be used to endorse
 *    or promote products derived from this software without specific prior written permission.
 *
 * 4. This software, with or without modification, must only be used with a
 *    Sifli integrated circuit.
 *
 * 5. Any software provided in binary form under this license must not be reverse
 *    engineered, decompiled, modified and/or disassembled.
 *
 * THIS SOFTWARE IS PROVIDED BY SIFLI TECHNOLOGY "AS IS" AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY, NONINFRINGEMENT, AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL SIFLI TECHNOLOGY OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "sdcard.h"
#include "sdcard_port.h"
#include <rthw.h>
#include <rtdevice.h>
#include <dfs_fs.h>
#include <dfs_file.h>
#include <string.h>

#define DBG_TAG "sdcard"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/* Sampling and probe budgets follow Solution's TF worker. */
#define SDCARD_SETTLE_MS       15
#define SDCARD_CONFIRM_MS      3
#define SDCARD_POLL_MS         30
#define SDCARD_PROBE_POLLS     1000
#define SDCARD_REMOVE_POLLS    100
/* Platform baseline: report slow release, then keep waiting cooperatively. */
#define SDCARD_RELEASE_MS      35000
#define SDCARD_STACK_SIZE      3072
#define SDCARD_PRIORITY        22

static struct
{
    sdcard_config_t config;
    sdcard_snapshot_t view;
    struct rt_mmcsd_host *host;
    struct rt_semaphore event;
    struct rt_semaphore released;
    rt_thread_t worker;
    uint32_t edges;
    uint32_t removals;
    uint32_t fault_removals;
    uint32_t observed_removals;
    uint32_t next_release;
    rt_bool_t invalidated;
    rt_bool_t wake_pending;
    rt_bool_t acknowledged;
    rt_bool_t fatal;
    rt_bool_t boot_checked;
    unsigned lifecycle; /* 0: stopped, 1: starting, 2: resident. */
} s_card;

static uint32_t _next_id(uint32_t value)
{
    ++value;
    return value ? value : 1;
}

static rt_bool_t _pin_present(void)
{
    return rt_pin_read(s_card.config.detect_pin) == s_card.config.present_level;
}

static void _invalidate_locked(void)
{
    s_card.invalidated = RT_TRUE;
    if (s_card.view.available)
    {
        s_card.view.available = RT_FALSE;
        s_card.view.session = _next_id(s_card.view.session);
        ++s_card.view.revision;
    }
}

static void _wake_worker(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    rt_bool_t post = !s_card.wake_pending;
    s_card.wake_pending = RT_TRUE;
    rt_hw_interrupt_enable(level);
    if (post)
    {
        rt_sem_release(&s_card.event);
    }
}

static void _pin_irq(void *argument)
{
    rt_base_t level;
    (void)argument;
    level = rt_hw_interrupt_disable();
    ++s_card.edges;
    if (!_pin_present())
    {
        ++s_card.removals;
        _invalidate_locked();
    }
    rt_hw_interrupt_enable(level);
    _wake_worker();
}

static void _set_state(sdcard_state_t state, sdcard_error_stage_t stage, rt_err_t error)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (s_card.view.state != state || s_card.view.error_stage != stage ||
        s_card.view.error != error)
    {
        s_card.view.state = state;
        s_card.view.error_stage = stage;
        s_card.view.error = error;
        ++s_card.view.revision;
    }
    rt_hw_interrupt_enable(level);
}

static void _fault(sdcard_error_stage_t stage, rt_err_t error, rt_bool_t fatal)
{
    rt_base_t level = rt_hw_interrupt_disable();
    _invalidate_locked();
    s_card.fatal = fatal;
    s_card.fault_removals = s_card.observed_removals;
    rt_hw_interrupt_enable(level);
    _set_state(SDCARD_FAULT, stage, error);
    LOG_W("stage=%d error=%d isolated=%d", stage, error, fatal);
}

static uint32_t _sample(rt_bool_t *present)
{
    uint32_t edges;
    rt_bool_t first, second;
    rt_base_t level;

    for (;;)
    {
        level = rt_hw_interrupt_disable();
        edges = s_card.edges;
        rt_hw_interrupt_enable(level);
        rt_thread_mdelay(SDCARD_SETTLE_MS);
        first = _pin_present();
        rt_thread_mdelay(SDCARD_CONFIRM_MS);
        second = _pin_present();
        level = rt_hw_interrupt_disable();
        if (first == second && edges == s_card.edges)
        {
            if (s_card.view.present != second)
            {
                s_card.view.present = second;
                ++s_card.view.revision;
            }
            if (!second)
            {
                _invalidate_locked();
            }
            *present = second;
            s_card.observed_removals = s_card.removals;
            rt_hw_interrupt_enable(level);
            return edges;
        }
        rt_hw_interrupt_enable(level);
    }
}

static rt_bool_t _current(uint32_t edges)
{
    rt_base_t level = rt_hw_interrupt_disable();
    rt_bool_t current = edges == s_card.edges && _pin_present();
    rt_hw_interrupt_enable(level);
    return current;
}

static rt_err_t _wait_change(unsigned polls, sdcard_error_stage_t stage,
                            sdcard_port_status_t *status)
{
    sdcard_port_get_status(s_card.host, status);
    while (status->change == SDCARD_CHANGE_PENDING && polls)
    {
        --polls;
        rt_thread_mdelay(SDCARD_POLL_MS);
        sdcard_port_get_status(s_card.host, status);
    }
    if (status->change == SDCARD_CHANGE_PENDING)
    {
        /* A timeout does not cancel the SDK operation. No further requests. */
        _fault(stage, -RT_ETIMEOUT, RT_TRUE);
        return -RT_ETIMEOUT;
    }
    return RT_EOK;
}

static rt_bool_t _has_open_files(void)
{
    rt_bool_t busy = RT_FALSE;
    struct dfs_fdtable *table;
    unsigned i;

    dfs_lock();
    table = dfs_fdtable_get();
    for (i = 0; i < table->maxfd; ++i)
    {
        struct dfs_fd *fd = table->fds[i];
        if (fd && fd->ref_count && fd->fs && fd->fs->path &&
            strcmp(fd->fs->path, s_card.config.mount_point) == 0)
        {
            busy = RT_TRUE;
            break;
        }
    }
    dfs_unlock();
    return busy;
}

static rt_err_t _dfs_error(void)
{
    rt_err_t error = rt_get_errno();
    return error ? (error < 0 ? error : -error) : -RT_ERROR;
}

static void _set_mounted(rt_bool_t mounted)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (s_card.view.mounted != mounted)
    {
        s_card.view.mounted = mounted;
        ++s_card.view.revision;
    }
    rt_hw_interrupt_enable(level);
}

static void _retire(void)
{
    rt_err_t error;
    rt_base_t level;
    sdcard_port_status_t status;

    level = rt_hw_interrupt_disable();
    _invalidate_locked();
    s_card.next_release = _next_id(s_card.next_release);
    s_card.view.release_id = s_card.next_release;
    s_card.acknowledged = RT_FALSE;
    s_card.view.state = SDCARD_DRAINING;
    s_card.view.error_stage = SDCARD_ERROR_NONE;
    s_card.view.error = RT_EOK;
    ++s_card.view.revision;
    rt_hw_interrupt_enable(level);

    /* No I/O or host lock is held while the platform releases its users. */
    error = rt_sem_take(&s_card.released, rt_tick_from_millisecond(SDCARD_RELEASE_MS));
    if (error == -RT_ETIMEOUT)
    {
        _set_state(SDCARD_DRAINING, SDCARD_ERROR_RELEASE_WAIT, error);
        error = rt_sem_take(&s_card.released, RT_WAITING_FOREVER);
    }
    if (error != RT_EOK)
    {
        _fault(SDCARD_ERROR_RELEASE_WAIT, error, RT_TRUE);
        return;
    }

    _set_state(SDCARD_UNMOUNTING, SDCARD_ERROR_NONE, RT_EOK);
    error = rt_mutex_take(s_card.config.io_lock, RT_WAITING_FOREVER);
    if (error != RT_EOK)
    {
        _fault(SDCARD_ERROR_UNMOUNT, error, RT_TRUE);
        return;
    }
    if (_has_open_files())
    {
        rt_mutex_release(s_card.config.io_lock);
        _fault(SDCARD_ERROR_OPEN_FILES, -RT_EBUSY, RT_TRUE);
        return;
    }
    if (s_card.view.mounted)
    {
        rt_set_errno(0);
        if (dfs_unmount(s_card.config.mount_point) != 0)
        {
            error = _dfs_error();
            rt_mutex_release(s_card.config.io_lock);
            _fault(SDCARD_ERROR_UNMOUNT, error, RT_TRUE);
            return;
        }
        _set_mounted(RT_FALSE);
    }
    rt_mutex_release(s_card.config.io_lock);

    sdcard_port_get_status(s_card.host, &status);
    if (status.change == SDCARD_CHANGE_PENDING)
    {
        _fault(SDCARD_ERROR_REMOVE_SUBMIT, -RT_EBUSY, RT_TRUE);
        return;
    }
    if (status.has_card)
    {
        error = sdcard_port_request(s_card.host);
        if (error != RT_EOK)
        {
            _fault(SDCARD_ERROR_REMOVE_SUBMIT, error, RT_TRUE);
            return;
        }
        if (_wait_change(SDCARD_REMOVE_POLLS, SDCARD_ERROR_REMOVE_WAIT, &status) != RT_EOK)
        {
            return;
        }
    }
    if (status.has_card || status.block_device)
    {
        _fault(SDCARD_ERROR_REMOVE_RESULT, -RT_ERROR, RT_TRUE);
        return;
    }
    level = rt_hw_interrupt_disable();
    s_card.view.release_id = 0;
    s_card.invalidated = RT_FALSE;
    ++s_card.view.revision;
    rt_hw_interrupt_enable(level);
    _set_state(SDCARD_ABSENT, SDCARD_ERROR_NONE, RT_EOK);
    _wake_worker(); /* Reconcile an insertion received while releasing the old card. */
}

static void _reconcile(void)
{
    rt_bool_t present;
    rt_bool_t adopted;
    uint32_t edges;
    rt_base_t level;
    rt_err_t error;
    sdcard_port_status_t status;

    if (s_card.fatal)
    {
        return;
    }
    edges = _sample(&present);
    level = rt_hw_interrupt_disable();
    if (s_card.view.state == SDCARD_FAULT && present &&
        s_card.fault_removals == s_card.removals)
    {
        rt_hw_interrupt_enable(level);
        return; /* Probe/mount failures require removal before another attempt. */
    }
    rt_hw_interrupt_enable(level);

    sdcard_port_get_status(s_card.host, &status);
    adopted = !s_card.boot_checked && status.change != SDCARD_CHANGE_NONE;
    s_card.boot_checked = RT_TRUE;
    if (status.change == SDCARD_CHANGE_PENDING)
    {
        adopted = RT_TRUE;
        _set_state(SDCARD_PROBING, SDCARD_ERROR_NONE, RT_EOK);
        if (_wait_change(SDCARD_PROBE_POLLS, SDCARD_ERROR_PROBE_WAIT, &status) != RT_EOK)
        {
            return;
        }
    }
    level = rt_hw_interrupt_disable();
    if (edges != s_card.edges)
    {
        _invalidate_locked();
    }
    present = _pin_present();
    if ((s_card.invalidated || !present) && (status.has_card || s_card.view.mounted))
    {
        rt_hw_interrupt_enable(level);
        _retire();
        return;
    }
    if (!present)
    {
        s_card.invalidated = RT_FALSE;
        rt_hw_interrupt_enable(level);
        _set_state(SDCARD_ABSENT, SDCARD_ERROR_NONE, RT_EOK);
        return;
    }
    if (edges != s_card.edges)
    {
        rt_hw_interrupt_enable(level);
        _wake_worker();
        return;
    }
    if (s_card.view.available)
    {
        rt_hw_interrupt_enable(level);
        return;
    }
    s_card.invalidated = RT_FALSE;
    rt_hw_interrupt_enable(level);

    _set_state(SDCARD_PROBING, SDCARD_ERROR_NONE, RT_EOK);
    if (!status.has_card && !adopted)
    {
        error = sdcard_port_request(s_card.host);
        if (error != RT_EOK)
        {
            _fault(SDCARD_ERROR_PROBE_SUBMIT, error, RT_FALSE);
            return;
        }
        if (_wait_change(SDCARD_PROBE_POLLS, SDCARD_ERROR_PROBE_WAIT, &status) != RT_EOK)
        {
            return;
        }
    }
    if (!_current(edges))
    {
        level = rt_hw_interrupt_disable();
        _invalidate_locked();
        rt_hw_interrupt_enable(level);
        if (status.has_card)
        {
            _retire();
        }
        else
        {
            _wake_worker();
        }
        return;
    }
    if (!status.has_card || !status.sd_memory || !status.block_device)
    {
        _fault(SDCARD_ERROR_PROBE_RESULT, -RT_ERROR, RT_FALSE);
        return;
    }

    error = rt_mutex_take(s_card.config.io_lock, RT_WAITING_FOREVER);
    if (error != RT_EOK)
    {
        _fault(SDCARD_ERROR_MOUNT, error, RT_FALSE);
        return;
    }
    if (!_current(edges))
    {
        rt_mutex_release(s_card.config.io_lock);
        _retire();
        return;
    }
    rt_set_errno(0);
    error = dfs_mount(s_card.config.device_name, s_card.config.mount_point, "elm", 0, RT_NULL);
    if (error != 0)
    {
        error = _dfs_error();
    }
    else
    {
        _set_mounted(RT_TRUE);
    }
    rt_mutex_release(s_card.config.io_lock);

    level = rt_hw_interrupt_disable();
    if (edges != s_card.edges || !_pin_present())
    {
        _invalidate_locked();
        rt_hw_interrupt_enable(level);
        _retire();
        return;
    }
    if (error != RT_EOK)
    {
        rt_hw_interrupt_enable(level);
        _fault(SDCARD_ERROR_MOUNT, error, RT_FALSE);
        return;
    }
    s_card.view.available = RT_TRUE;
    s_card.view.session = _next_id(s_card.view.session);
    s_card.view.state = SDCARD_READY;
    s_card.view.error_stage = SDCARD_ERROR_NONE;
    s_card.view.error = RT_EOK;
    ++s_card.view.revision;
    rt_hw_interrupt_enable(level);
    LOG_I("mounted %s", s_card.config.mount_point);
}

static void _worker(void *argument)
{
    rt_base_t level;
    (void)argument;
    for (;;)
    {
        rt_sem_take(&s_card.event, RT_WAITING_FOREVER);
        level = rt_hw_interrupt_disable();
        s_card.wake_pending = RT_FALSE;
        rt_hw_interrupt_enable(level);
        _reconcile();
    }
}

void sdcard_get_snapshot(sdcard_snapshot_t *snapshot)
{
    rt_base_t level;
    RT_ASSERT(snapshot);
    level = rt_hw_interrupt_disable();
    *snapshot = s_card.view;
    rt_hw_interrupt_enable(level);
}

rt_err_t sdcard_release_complete(uint32_t release_id)
{
    rt_err_t error;
    rt_base_t level = rt_hw_interrupt_disable();
    if (s_card.lifecycle != 2 || s_card.view.state != SDCARD_DRAINING ||
        !release_id || release_id != s_card.view.release_id)
    {
        rt_hw_interrupt_enable(level);
        return -RT_EINVAL;
    }
    if (s_card.acknowledged)
    {
        rt_hw_interrupt_enable(level);
        return -RT_EBUSY;
    }
    s_card.acknowledged = RT_TRUE;
    rt_hw_interrupt_enable(level);
    error = rt_sem_release(&s_card.released);
    if (error != RT_EOK)
    {
        level = rt_hw_interrupt_disable();
        s_card.acknowledged = RT_FALSE;
        rt_hw_interrupt_enable(level);
    }
    return error;
}

static rt_err_t _check_mount_owner(void)
{
    struct dfs_filesystem *fs;
    rt_device_t device;
    char *path;
    rt_err_t error;

    path = dfs_normalize_path(RT_NULL, s_card.config.mount_point);
    if (!path)
    {
        return -RT_ENOMEM;
    }
    error = strcmp(path, s_card.config.mount_point) == 0 ? RT_EOK : -RT_EINVAL;
    rt_free(path);
    if (error != RT_EOK)
    {
        return error;
    }
    error = rt_mutex_take(s_card.config.io_lock, RT_WAITING_FOREVER);
    if (error != RT_EOK)
    {
        return error;
    }
    device = rt_device_find(s_card.config.device_name);
    if (device && dfs_filesystem_get_mounted_path(device))
    {
        error = -RT_EBUSY;
    }
    fs = dfs_filesystem_lookup(s_card.config.mount_point);
    if (fs && strcmp(fs->path, s_card.config.mount_point) == 0)
    {
        error = -RT_EBUSY;
    }
    rt_mutex_release(s_card.config.io_lock);
    return error;
}

rt_err_t sdcard_start(const sdcard_config_t *config)
{
    rt_err_t error;
    rt_base_t level;

    if (!config || !config->device_name || !config->device_name[0] ||
        !config->mount_point || config->mount_point[0] != '/' || !config->mount_point[1] ||
        !config->io_lock || config->detect_pin != SD_INSERT_DETECT_PIN ||
        config->present_level != PIN_LOW)
    {
        return -RT_EINVAL;
    }
    level = rt_hw_interrupt_disable();
    if (s_card.lifecycle)
    {
        rt_hw_interrupt_enable(level);
        return -RT_EBUSY;
    }
    rt_memset(&s_card, 0, sizeof(s_card));
    s_card.lifecycle = 1;
    s_card.config = *config;
    rt_hw_interrupt_enable(level);

    error = sdcard_port_bind(config->device_name, &s_card.host);
    if (error != RT_EOK)
    {
        goto fail;
    }
    error = _check_mount_owner();
    if (error != RT_EOK)
    {
        goto fail;
    }
    error = rt_sem_init(&s_card.event, "sd_event", 0, RT_IPC_FLAG_FIFO);
    if (error != RT_EOK)
    {
        goto fail;
    }
    error = rt_sem_init(&s_card.released, "sd_release", 0, RT_IPC_FLAG_FIFO);
    if (error != RT_EOK)
    {
        goto fail_event;
    }
    s_card.worker = rt_thread_create("sd_card", _worker, RT_NULL,
                                      SDCARD_STACK_SIZE, SDCARD_PRIORITY, 10);
    if (!s_card.worker)
    {
        error = -RT_ENOMEM;
        goto fail_release;
    }
    error = rt_pin_attach_irq(config->detect_pin, PIN_IRQ_MODE_RISING_FALLING, _pin_irq, RT_NULL);
    if (error != RT_EOK)
    {
        goto fail_thread;
    }
    rt_pin_mode(config->detect_pin, PIN_MODE_INPUT);
    error = rt_pin_irq_enable(config->detect_pin, PIN_IRQ_ENABLE);
    if (error != RT_EOK)
    {
        goto fail_irq;
    }
    level = rt_hw_interrupt_disable();
    s_card.lifecycle = 2;
    rt_hw_interrupt_enable(level);
    error = rt_thread_startup(s_card.worker);
    if (error != RT_EOK)
    {
        goto fail_irq;
    }
    _wake_worker();
    return RT_EOK;

fail_irq:
    rt_pin_irq_enable(config->detect_pin, PIN_IRQ_DISABLE);
    rt_pin_detach_irq(config->detect_pin);
fail_thread:
    rt_thread_delete(s_card.worker);
    s_card.worker = RT_NULL;
fail_release:
    rt_sem_detach(&s_card.released);
fail_event:
    rt_sem_detach(&s_card.event);
fail:
    _set_state(SDCARD_FAULT, SDCARD_ERROR_START, error);
    level = rt_hw_interrupt_disable();
    s_card.lifecycle = 0;
    rt_hw_interrupt_enable(level);
    return error;
}
