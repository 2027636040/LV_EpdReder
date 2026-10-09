/* SPDX-License-Identifier: Apache-2.0 */
#include "hang_diag.h"
#include <rthw.h>
#include <rtdevice.h>
#include <drivers/serial.h>
#include <stdarg.h>
#include <string.h>
#include "lvgl.h"

#if !defined(RT_USING_HOOK) || defined(USING_CPU_USAGE_PROFILER) || defined(PKG_USING_SYSTEMVIEW)
#error "HANG_DIAG requires the RT-Thread scheduler hook without another profiler"
#endif

#define DIAG_THREADS 48
#define DIAG_TIMEOUT_MS 10000
#define DIAG_REPORT_LIMIT 3

typedef struct
{
    rt_thread_t thread;
    char name[RT_NAME_MAX + 1];
    rt_tick_t ticks;
    const char *phase;
    rt_tick_t phase_since;
} thread_sample_t;

typedef struct
{
    const char *stage;
    rt_tick_t since;
    rt_uint32_t detail;
    rt_bool_t active;
} stage_sample_t;

typedef struct
{
    rt_thread_t thread;
    char name[RT_NAME_MAX + 1];
    rt_tick_t ticks;
    const char *phase;
    rt_tick_t phase_age;
    void *sp;
    rt_uint8_t state;
    rt_uint8_t priority;
    rt_uint8_t base_priority;
    const char *wait_type;
    char wait_name[RT_NAME_MAX + 1];
    char owner[RT_NAME_MAX + 1];
} thread_snapshot_t;

static thread_sample_t samples[DIAG_THREADS];
static thread_snapshot_t snapshot[DIAG_THREADS];
static stage_sample_t stages[HANG_DIAG_CHANNELS];
static struct rt_thread monitor;
ALIGN(RT_ALIGN_SIZE)
static rt_uint8_t monitor_stack[3072];
static struct rt_serial_device *console;
static rt_tick_t switch_tick;
static rt_bool_t enabled;
static rt_bool_t table_full;
static char preempted[RT_NAME_MAX + 1];

/* All sample-table access is protected by the single-core scheduler IRQ lock. */
static thread_sample_t *sample_for(rt_thread_t thread)
{
    thread_sample_t *empty = RT_NULL;
    if (!thread) return RT_NULL;
    for (unsigned i = 0; i < DIAG_THREADS; ++i)
    {
        thread_sample_t *s = &samples[i];
        if (s->thread == thread)
        {
            if (memcmp(s->name, thread->name, RT_NAME_MAX) != 0)
            {
                memset(s, 0, sizeof(*s));
                s->thread = thread;
                memcpy(s->name, thread->name, RT_NAME_MAX);
            }
            return s;
        }
        if (!s->thread && !empty) empty = s;
    }
    if (empty)
    {
        empty->thread = thread;
        memcpy(empty->name, thread->name, RT_NAME_MAX);
    }
    else table_full = RT_TRUE;
    return empty;
}

static void scheduled(rt_thread_t from, rt_thread_t to)
{
    rt_tick_t now = rt_tick_get();
    thread_sample_t *s = sample_for(from);
    if (s) s->ticks += now - switch_tick;
    switch_tick = now;
    if (to == &monitor && from) memcpy(preempted, from->name, RT_NAME_MAX);
}

void hang_diag_mark(hang_diag_channel_t channel, const char *stage,
                    rt_uint32_t detail, rt_bool_t active)
{
    if (!enabled) return;
    rt_base_t level = rt_hw_interrupt_disable();
    stages[channel].stage = stage;
    stages[channel].since = rt_tick_get();
    stages[channel].detail = detail;
    stages[channel].active = active;
    rt_hw_interrupt_enable(level);
}

static const char *phase_enter(const char *phase)
{
    const char *previous = RT_NULL;
    if (!enabled) return previous;
    rt_base_t level = rt_hw_interrupt_disable();
    thread_sample_t *s = sample_for(rt_thread_self());
    if (s)
    {
        previous = s->phase;
        s->phase = phase;
        s->phase_since = rt_tick_get();
    }
    rt_hw_interrupt_enable(level);
    return previous;
}

/* Bypass ulog's lock and buffer, which may be held by the stalled thread.
 * The board UART putc polls the transmitter; no shell thread is involved. */
static void diag_print(const char *format, ...)
{
    char line[224];
    va_list args;
    va_start(args, format);
    int length = rt_vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (length < 0) return;
    if ((unsigned)length >= sizeof(line)) length = sizeof(line) - 1;
    for (int i = 0; i < length; ++i)
    {
        if (line[i] == '\n') console->ops->putc(console, '\r');
        console->ops->putc(console, line[i]);
    }
}

static void snapshot_waiters(rt_list_t *list, unsigned count,
                             const char *type, struct rt_object *object,
                             rt_thread_t owner)
{
    unsigned visited = 0;
    for (rt_list_t *n = list->next; n != list && visited < DIAG_THREADS; n = n->next, ++visited)
    {
        rt_thread_t thread = rt_list_entry(n, struct rt_thread, tlist);
        for (unsigned i = 0; i < count; ++i)
        {
            thread_snapshot_t *s = &snapshot[i];
            if (s->thread != thread) continue;
            s->wait_type = type;
            memcpy(s->wait_name, object->name, RT_NAME_MAX);
            if (owner) memcpy(s->owner, owner->name, RT_NAME_MAX);
            break;
        }
    }
}

static void snapshot_ipc(enum rt_object_class_type type, const char *name, unsigned count)
{
    struct rt_object_information *info = rt_object_get_information(type);
    if (!info) return;
    unsigned visited = 0;
    for (rt_list_t *n = info->object_list.next;
         n != &info->object_list && visited < 256; n = n->next, ++visited)
    {
        struct rt_object *object = rt_list_entry(n, struct rt_object, list);
        struct rt_ipc_object *ipc = (struct rt_ipc_object *)object;
        rt_thread_t owner = type == RT_Object_Class_Mutex ? ((rt_mutex_t)object)->owner : RT_NULL;
        snapshot_waiters(&ipc->suspend_thread, count, name, object, owner);
        if (type == RT_Object_Class_MailBox)
            snapshot_waiters(&((rt_mailbox_t)object)->suspend_sender_thread,
                             count, "mb-send", object, RT_NULL);
    }
}

static unsigned snapshot_threads(rt_tick_t now)
{
    unsigned count = 0;
    struct rt_object_information *info = rt_object_get_information(RT_Object_Class_Thread);
    memset(snapshot, 0, sizeof(snapshot));
    for (rt_list_t *n = info->object_list.next;
         n != &info->object_list && count < DIAG_THREADS; n = n->next)
    {
        rt_thread_t thread = rt_list_entry(n, struct rt_thread, list);
        thread_snapshot_t *s = &snapshot[count++];
        s->thread = thread;
        memcpy(s->name, thread->name, RT_NAME_MAX);
        s->state = thread->stat & RT_THREAD_STAT_MASK;
        s->priority = thread->current_priority;
        s->base_priority = thread->init_priority;
        s->sp = thread->sp;
        s->wait_type = "-";
        thread_sample_t *sample = sample_for(thread);
        if (sample)
        {
            s->ticks = sample->ticks;
            s->phase = sample->phase;
            s->phase_age = now - sample->phase_since;
        }
    }
    snapshot_ipc(RT_Object_Class_Semaphore, "sem", count);
    snapshot_ipc(RT_Object_Class_Mutex, "mutex", count);
    snapshot_ipc(RT_Object_Class_Event, "event", count);
    snapshot_ipc(RT_Object_Class_MailBox, "mb-recv", count);
    snapshot_ipc(RT_Object_Class_MessageQueue, "mq", count);
    return count;
}

static const char *state_name(rt_uint8_t state)
{
    switch (state)
    {
    case RT_THREAD_INIT: return "init";
    case RT_THREAD_READY: return "ready";
    case RT_THREAD_SUSPEND: return "wait";
    case RT_THREAD_RUNNING: return "run";
    case RT_THREAD_CLOSE: return "close";
    default: return "?";
    }
}

static void monitor_entry(void *parameter)
{
    (void)parameter;
    rt_tick_t previous = rt_tick_get();
    rt_tick_t last_report = previous;
    unsigned reports = 0;
    while (1)
    {
        rt_thread_mdelay(1000);
        stage_sample_t saved[HANG_DIAG_CHANNELS];
        rt_base_t level = rt_hw_interrupt_disable();
        rt_tick_t now = rt_tick_get();
        thread_sample_t *self = sample_for(&monitor);
        if (self) self->ticks += now - switch_tick;
        switch_tick = now;
        memcpy(saved, stages, sizeof(saved));
        rt_bool_t stalled = RT_FALSE;
        for (unsigned i = 0; i < HANG_DIAG_CHANNELS; ++i)
            if (saved[i].active && now - saved[i].since >= rt_tick_from_millisecond(DIAG_TIMEOUT_MS))
                stalled = RT_TRUE;
        rt_bool_t report = stalled && reports < DIAG_REPORT_LIMIT &&
                           (!reports || now - last_report >= rt_tick_from_millisecond(DIAG_TIMEOUT_MS));
        unsigned count = report ? snapshot_threads(now) : 0;
        for (unsigned i = 0; i < DIAG_THREADS; ++i) samples[i].ticks = 0;
        rt_tick_t window = now - previous;
        previous = now;
        rt_hw_interrupt_enable(level);

        if (!stalled)
        {
            if (reports) diag_print("[hang] progress resumed\n");
            reports = 0;
        }
        if (!report) continue;
        ++reports;
        last_report = now;
        diag_print("\n[hang] stall #%u tick=%u window_ticks=%u tick_hz=%u preempted=%s table_full=%u\n",
                   reports, now, window, RT_TICK_PER_SECOND, preempted, table_full);
        for (unsigned i = 0; i < HANG_DIAG_CHANNELS; ++i)
            diag_print("[hang] %s stage=%s active=%u age_ticks=%u detail=%u\n",
                       i == HANG_DIAG_UI ? "UI" : "LCD", saved[i].stage ? saved[i].stage : "none",
                       saved[i].active, now - saved[i].since, saved[i].detail);
        diag_print("[hang] scheduled ticks include interrupt time; SP is saved context, not live PC\n");
        for (unsigned i = 0; i < count; ++i)
        {
            thread_snapshot_t *s = &snapshot[i];
            diag_print("[hang] %s@%p %s prio=%u/%u ticks=%u sp=%p phase=%s age=%u\n",
                       s->name, s->thread, state_name(s->state), s->priority, s->base_priority,
                       s->ticks, s->sp, s->phase ? s->phase : "-", s->phase ? s->phase_age : 0);
            if (s->wait_type[0] != '-')
                diag_print("[hang]   waits %s:%s owner=%s\n", s->wait_type, s->wait_name,
                           s->owner[0] ? s->owner : "-");
        }
        diag_print("[hang] end\n");
    }
}

void hang_diag_init(void)
{
    rt_device_t device = rt_console_get_device();
    if (!device || device->type != RT_Device_Class_Char) return;
    console = (struct rt_serial_device *)device;
    rt_err_t result = rt_thread_init(&monitor, "hangdiag", monitor_entry, RT_NULL,
                                     monitor_stack, sizeof(monitor_stack), 1, 10);
    if (result != RT_EOK) return;
    rt_base_t level = rt_hw_interrupt_disable();
    switch_tick = rt_tick_get();
    rt_scheduler_sethook(scheduled);
    enabled = RT_TRUE;
    rt_hw_interrupt_enable(level);
    hang_diag_mark(HANG_DIAG_UI, "start", 0, RT_TRUE);
    diag_print("[hang] armed timeout_ms=%u reports=%u monitor_prio=1\n",
               DIAG_TIMEOUT_MS, DIAG_REPORT_LIMIT);
    rt_thread_startup(&monitor);
}

/* Link wrappers observe SDK draw calls without changing their implementation. */
void __real_lv_draw_epic_label(lv_draw_task_t *, const lv_draw_label_dsc_t *, const lv_area_t *);
void __wrap_lv_draw_epic_label(lv_draw_task_t *task, const lv_draw_label_dsc_t *dsc, const lv_area_t *area)
{
    const char *previous = phase_enter("epic-label");
    __real_lv_draw_epic_label(task, dsc, area);
    phase_enter(previous);
}

void __real_lv_draw_epic_img(lv_draw_task_t *, const lv_draw_image_dsc_t *, const lv_area_t *);
void __wrap_lv_draw_epic_img(lv_draw_task_t *task, const lv_draw_image_dsc_t *dsc, const lv_area_t *area)
{
    const char *previous = phase_enter("epic-image");
    __real_lv_draw_epic_img(task, dsc, area);
    phase_enter(previous);
}

rt_err_t __real_drv_gpu_check_done(rt_int32_t ms);
rt_err_t __wrap_drv_gpu_check_done(rt_int32_t ms)
{
    const char *previous = phase_enter("gpu-wait");
    rt_err_t result = __real_drv_gpu_check_done(ms);
    phase_enter(previous);
    return result;
}
