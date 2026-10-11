#include "app_service.h"
#include "net/network.h"
#include <dlmodule.h>
#include <dlfcn.h>
#include <rtm.h>
#include <rthw.h>
#include <string.h>
#include "platform/app_module.h"
#include "storage_file.h"

struct epd_service
{
    struct epd_service *next;
    char id[RT_NAME_MAX];
    epd_background_t definition;
    struct rt_event wakeup;
    uint32_t subscription;
    void *module;
    storage_volume_t volume;
    uint32_t storage_session;
    bool stopping, worker_done, ui_done;
    bool ui_requested;
    uint32_t ui_deadline;
    unsigned callers;
    char summary[96];
};
static epd_service_t *services;
static epd_service_t *ui_current;
static uint32_t summary_revision;

/* UI thread owns list changes; shell lookups hold an interrupt guard. */
static void service_add(epd_service_t *service)
{
    rt_base_t level = rt_hw_interrupt_disable();
    service->next = services;
    services = service;
    ++summary_revision;
    rt_hw_interrupt_enable(level);
}

static void service_remove(epd_service_t *service)
{
    rt_base_t level = rt_hw_interrupt_disable();
    epd_service_t **link = &services;
    while (*link && *link != service) link = &(*link)->next;
    if (*link)
    {
        *link = service->next;
        ++summary_revision;
    }
    rt_hw_interrupt_enable(level);
}

void epd_service_publish_summary(epd_service_t *service, const char *text)
{
    if (!service) return;
    char snapshot[sizeof(service->summary)];
    rt_strncpy(snapshot, text ? text : "", sizeof(snapshot) - 1);
    snapshot[sizeof(snapshot) - 1] = 0;
    rt_base_t level = rt_hw_interrupt_disable();
    if (!service->stopping && strcmp(service->summary, snapshot))
    {
        strcpy(service->summary, snapshot);
        ++summary_revision;
    }
    rt_hw_interrupt_enable(level);
}
RTM_EXPORT(epd_service_publish_summary);

void app_service_summary(const char *id, char *text, size_t capacity)
{
    (void)epd_app_summary_read(id, text, capacity);
}

uint32_t epd_app_summary_revision(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    uint32_t revision = summary_revision;
    rt_hw_interrupt_enable(level);
    return revision;
}
RTM_EXPORT(epd_app_summary_revision);

bool epd_app_summary_read(const char *id, char *text, size_t capacity)
{
    if (!text || !capacity) return false;
    text[0] = 0;
    if (!id) return false;
    bool found = false;
    rt_base_t level = rt_hw_interrupt_disable();
    for (epd_service_t *service = services; service; service = service->next)
        if (!service->stopping && !strcmp(service->id, id))
        {
            size_t length = strlen(service->summary);
            if (length >= capacity) length = capacity - 1;
            memcpy(text, service->summary, length);
            text[length] = 0;
            found = true;
            break;
        }
    rt_hw_interrupt_enable(level);
    return found;
}
RTM_EXPORT(epd_app_summary_read);

bool epd_service_cancelled(epd_service_t *service)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool stopping = service->stopping;
    rt_hw_interrupt_enable(level);
    return stopping || (service->module && !storage_session_valid(service->volume, service->storage_session));
}
RTM_EXPORT(epd_service_cancelled);

uint32_t epd_service_wait(epd_service_t *service, rt_int32_t ticks)
{
    uint32_t events = 0;
    if (epd_service_cancelled(service)) return EPD_SERVICE_STOP;
    rt_event_recv(&service->wakeup, EPD_SERVICE_WAKE | EPD_SERVICE_NETWORK | EPD_SERVICE_STOP,
                   RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, ticks, &events);
    return events;
}
RTM_EXPORT(epd_service_wait);

void epd_service_wake(epd_service_t *service)
{
    if (service) rt_event_send(&service->wakeup, EPD_SERVICE_WAKE);
}
RTM_EXPORT(epd_service_wake);

void epd_service_ui_request(epd_service_t *service, uint32_t delay_ms)
{
    if (!service || service != ui_current) return;
    uint64_t ticks = ((uint64_t)delay_ms * RT_TICK_PER_SECOND + 999u) / 1000u;
    if (ticks > INT32_MAX) ticks = INT32_MAX;
    uint32_t now = (uint32_t)rt_tick_get();
    uint32_t deadline = now + (uint32_t)ticks;
    rt_base_t level = rt_hw_interrupt_disable();
    if (!service->stopping && !service->ui_done &&
        (!service->ui_requested ||
         ((int32_t)(service->ui_deadline - now) > 0 &&
          (uint32_t)ticks < service->ui_deadline - now)))
    {
        service->ui_deadline = deadline;
        service->ui_requested = true;
    }
    rt_hw_interrupt_enable(level);
}
RTM_EXPORT(epd_service_ui_request);

uint32_t app_service_ui_delay(uint32_t default_ms)
{
    uint32_t now = (uint32_t)rt_tick_get();
    rt_base_t level = rt_hw_interrupt_disable();
    for (epd_service_t *service = services; service; service = service->next)
    {
        if (!service->ui_requested || service->stopping || service->ui_done) continue;
        int32_t ticks = (int32_t)(service->ui_deadline - now);
        uint64_t ms = ticks > 0 ? ((uint64_t)ticks * 1000u + RT_TICK_PER_SECOND - 1u) /
                                  RT_TICK_PER_SECOND : 0;
        if (ms < default_ms) default_ms = (uint32_t)ms;
    }
    rt_hw_interrupt_enable(level);
    return default_ms;
}

int app_service_command(const char *id, int argc, char **argv)
{
    epd_service_t *service = NULL;
    rt_base_t level = rt_hw_interrupt_disable();
    for (epd_service_t *item = services; item; item = item->next)
        if (!epd_service_cancelled(item) &&
            item->definition.command && !strcmp(item->id, id))
        {
            service = item;
            ++service->callers;
            break;
        }
    rt_hw_interrupt_enable(level);
    if (!service) return -RT_ENOSYS;
    int result = service->definition.command(argc, argv);
    level = rt_hw_interrupt_disable();
    --service->callers;
    /* Send while protected: the worker may publish done immediately afterward. */
    rt_event_send(&service->wakeup, EPD_SERVICE_WAKE);
    rt_hw_interrupt_enable(level);
    return result;
}

static void run(void *parameter)
{
    epd_service_t *service = parameter;
    service->definition.run(service);
    rt_base_t guard = rt_hw_interrupt_disable();
    if (!service->stopping) ++summary_revision;
    service->stopping = true;
    service->ui_requested = false;
    rt_hw_interrupt_enable(guard);
    for (;;)
    {
        guard = rt_hw_interrupt_disable();
        bool idle = service->callers == 0;
        rt_hw_interrupt_enable(guard);
        if (idle) break;
        uint32_t events;
        rt_event_recv(&service->wakeup, EPD_SERVICE_WAKE, RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                      RT_WAITING_FOREVER, &events);
    }
    network_unsubscribe(service->subscription);
    service->subscription = 0;
    /* This wrapper is resident firmware. After publishing done it never touches
     * module code, module data, or the service allocation again. */
    rt_base_t level = rt_hw_interrupt_disable();
    service->worker_done = true;
    rt_hw_interrupt_enable(level);
}

bool app_service_running(const char *id)
{
    for (epd_service_t *service = services; service; service = service->next)
        if (!strcmp(service->id, id)) return true;
    return false;
}

epd_service_t *app_service_start(const char *id, const epd_background_t *definition, void *module)
{
    if (!id || !*id || strlen(id) >= RT_NAME_MAX || !definition ||
        (!definition->run && !definition->ui_process) || app_service_running(id)) return NULL;
    if (definition->run && (definition->stack_size < 2048 || definition->stack_size > 32768 ||
        definition->priority < 20 || definition->priority >= RT_THREAD_PRIORITY_MAX)) return NULL;
    if (!definition->run && definition->network) return NULL;
    epd_service_t *service = rt_calloc(1, sizeof(*service));
    if (!service) return NULL;
    strcpy(service->id, id);
    service->definition = *definition;
    service->worker_done = definition->run == NULL;
    service->ui_done = definition->ui_process == NULL;
    if (rt_event_init(&service->wakeup, "app_evt", RT_IPC_FLAG_FIFO) != RT_EOK)
    { rt_free(service); return NULL; }
    if (definition->network)
    {
        service->subscription = network_subscribe(&service->wakeup, EPD_SERVICE_NETWORK);
        if (!service->subscription) goto failed;
    }
    if (module)
    {
        if (!app_module_storage(id, &service->volume, &service->storage_session) ||
            !storage_session_valid(service->volume, service->storage_session)) goto failed;
        char path[96], name[RT_NAME_MAX + 4];
        rt_snprintf(name, sizeof(name), "%s.so", id);
        if (!storage_app_path(path, sizeof(path), id, STORAGE_APP_CODE, name)) goto failed;
        service->module = app_module_open(path, id); /* SDK nref pins code past ONSTOP. */
        if (!service->module) goto failed;
    }
    if (!definition->run)
    {
        service_add(service);
        return service;
    }
    rt_thread_t thread = rt_thread_create(id, run, service, definition->stack_size, definition->priority, 10);
    if (!thread) goto failed;
    service_add(service);
    if (rt_thread_startup(thread) == RT_EOK) return service;
    service_remove(service);
    rt_thread_delete(thread);
failed:
    network_unsubscribe(service->subscription);
    if (service->module) app_module_close(service->module);
    rt_event_detach(&service->wakeup);
    rt_free(service);
    return NULL;
}

void app_service_stop(const char *id)
{
    for (epd_service_t *service = services; service; service = service->next)
    {
        if (strcmp(service->id, id)) continue;
        rt_base_t level = rt_hw_interrupt_disable();
        if (!service->stopping) ++summary_revision;
        service->stopping = true;
        service->ui_requested = false;
        rt_hw_interrupt_enable(level);
        rt_event_send(&service->wakeup, EPD_SERVICE_STOP);
    }
}

void app_service_process(void)
{
    epd_service_t *next;
    for (epd_service_t *service = services; service; service = next)
    {
        next = service->next;
        rt_base_t level = rt_hw_interrupt_disable();
        bool stopping = service->stopping;
        unsigned callers = service->callers;
        bool worker_done = service->worker_done;
        rt_hw_interrupt_enable(level);
        if (!stopping && epd_service_cancelled(service))
        {
            app_service_stop(service->id);
            stopping = true;
        }
        if (service->definition.ui_process && !service->ui_done)
        {
            /* UI cleanup cannot race a worker or a command still in module code. */
            if (stopping && (!worker_done || callers)) continue;
            level = rt_hw_interrupt_disable();
            service->ui_requested = false;
            rt_hw_interrupt_enable(level);
            ui_current = service;
            bool done = service->definition.ui_process(service, stopping);
            ui_current = NULL;
            RT_ASSERT(!done || stopping);
            if (done)
            {
                level = rt_hw_interrupt_disable();
                service->stopping = service->ui_done = true;
                service->ui_requested = false;
                rt_hw_interrupt_enable(level);
            }
        }
        if (!worker_done || !service->ui_done || callers) continue;
        service_remove(service);
        rt_event_detach(&service->wakeup);
        if (service->module) app_module_close(service->module);
        rt_free(service);
    }
}

bool app_service_stopping(void)
{
    for (epd_service_t *service = services; service; service = service->next)
        if (service->stopping) return true;
    return false;
}

bool app_service_flush(void)
{
    bool ok = true;
    for (epd_service_t *service = services; service; service = service->next)
    {
        if ((!service->worker_done || !service->ui_done) && service->definition.flush &&
            !service->definition.flush()) ok = false;
    }
    return ok;
}

void app_service_stop_modules(void)
{
    for (epd_service_t *service = services; service; service = service->next)
        if (service->module) app_service_stop(service->id);
}

bool app_service_modules_running(void)
{
    for (epd_service_t *service = services; service; service = service->next)
        if (service->module) return true;
    return false;
}
