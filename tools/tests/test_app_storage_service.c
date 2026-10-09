#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RT_NAME_MAX 16
#define RT_THREAD_PRIORITY_MAX 32
#define RT_EOK 0
#define RT_ENOSYS 38
#define RT_WAITING_FOREVER (-1)
#define RT_IPC_FLAG_FIFO 0
#define RT_EVENT_FLAG_OR 1
#define RT_EVENT_FLAG_CLEAR 2
#define RT_ASSERT assert
#define RTM_EXPORT(name)
#define rt_calloc calloc
#define rt_free free
#define rt_strncpy strncpy
#define rt_snprintf snprintf
#define EPD_SERVICE_WAKE 1u
#define EPD_SERVICE_NETWORK 2u
#define EPD_SERVICE_STOP 4u
typedef int rt_base_t;
typedef int32_t rt_int32_t;
typedef enum { STORAGE_FLASH, STORAGE_SD } storage_volume_t;
enum { STORAGE_APP_CODE };
struct rt_event { uint32_t events; };
typedef struct epd_service epd_service_t;
typedef struct
{
    void (*run)(epd_service_t *);
    uint32_t stack_size;
    uint8_t priority;
    bool network;
    int (*command)(int, char **);
    bool (*ui_process)(epd_service_t *, bool);
    bool (*flush)(void);
} epd_background_t;
typedef struct { void (*entry)(void *); void *parameter; } thread_t;
typedef thread_t *rt_thread_t;
static thread_t mock_thread;
static storage_volume_t install_volume = STORAGE_SD;
static uint32_t card_session = 7;
static bool card_available = true, ui_ready;
static unsigned pins, subscriptions, detached, cleanup_calls;
static bool fail_thread_start;

static rt_base_t rt_hw_interrupt_disable(void) { return 0; }
static void rt_hw_interrupt_enable(rt_base_t level) { (void)level; }
static int rt_event_init(struct rt_event *event, const char *name, int flags)
{ (void)name; (void)flags; event->events = 0; return RT_EOK; }
static void rt_event_send(struct rt_event *event, uint32_t bits) { event->events |= bits; }
static void rt_event_recv(struct rt_event *event, uint32_t bits, int flags, int ticks, uint32_t *out)
{ (void)flags; (void)ticks; *out = event->events & bits; event->events &= ~bits; }
static void rt_event_detach(struct rt_event *event) { (void)event; ++detached; }
static uint32_t network_subscribe(struct rt_event *event, uint32_t bits)
{ (void)event; (void)bits; ++subscriptions; return 1; }
static void network_unsubscribe(uint32_t id) { if (id) { assert(subscriptions); --subscriptions; } }
static bool app_module_storage(const char *id, storage_volume_t *volume, uint32_t *session)
{ (void)id; *volume = install_volume; *session = card_session; return true; }
static bool storage_session_valid(storage_volume_t volume, uint32_t session)
{ return volume == STORAGE_FLASH || (card_available && session == card_session); }
static bool storage_app_path(char *path, size_t capacity, const char *id, int area, const char *name)
{ (void)id; (void)area; snprintf(path, capacity, "/sdcard/%s", name); return true; }
static void *app_module_open(const char *path, const char *id)
{ (void)path; (void)id; ++pins; return &pins; }
static void app_module_close(void *module) { assert(module == &pins && pins); --pins; }
static rt_thread_t rt_thread_create(const char *id, void (*entry)(void *), void *argument,
                                   uint32_t stack, uint8_t priority, int slice)
{ (void)id; (void)stack; (void)priority; (void)slice; mock_thread.entry = entry; mock_thread.parameter = argument; return &mock_thread; }
static int rt_thread_startup(rt_thread_t value) { (void)value; return fail_thread_start ? -1 : 0; }
static void rt_thread_delete(rt_thread_t value) { (void)value; }

#include "service_under_test.h"

static void module_worker(epd_service_t *service)
{
    assert(epd_service_cancelled(service));
    assert(pins == 1 && subscriptions == 1);
    /* Worker still executes module code: UI cleanup must not run yet. */
    unsigned previous = cleanup_calls;
    app_service_process();
    assert(cleanup_calls == previous && app_service_running("books"));
}

static bool module_ui(epd_service_t *service, bool stopping)
{
    (void)service;
    if (!stopping) return false;
    ++cleanup_calls;
    assert(pins == 1 && subscriptions == 0);
    return ui_ready;
}

static int module_command(int argc, char **argv)
{
    (void)argc; (void)argv;
    app_service_stop("books");
    app_service_process();
    assert(pins == 1 && services && services->callers == 1);
    return 42;
}

int main(void)
{
    epd_background_t definition = {module_worker, 4096, 22, true, NULL, module_ui, NULL};
    epd_service_t *service = app_service_start("books", &definition, &pins);
    assert(service && pins == 1 && subscriptions == 1);
    card_available = false;
    assert(epd_service_cancelled(service));
    assert(epd_service_wait(service, 0) == EPD_SERVICE_STOP);
    app_service_process();
    assert(!cleanup_calls && pins == 1);
    mock_thread.entry(mock_thread.parameter);
    assert(!subscriptions && pins == 1);
    app_service_process();
    assert(cleanup_calls == 1 && app_service_modules_running());
    ui_ready = true;
    app_service_process();
    assert(!services && !pins && detached == 1);

    /* Internal modules reading card files are also in the global exit scope. */
    install_volume = STORAGE_FLASH;
    ui_ready = false;
    service = app_service_start("books", &definition, &pins);
    assert(service && !epd_service_cancelled(service));
    app_service_stop_modules();
    assert(epd_service_cancelled(service));
    mock_thread.entry(mock_thread.parameter);
    ui_ready = true;
    app_service_process();
    assert(!services && !pins && !subscriptions);

    /* A replacement session never makes an old service valid again. */
    install_volume = STORAGE_SD;
    card_available = true;
    service = app_service_start("books", &definition, &pins);
    ++card_session;
    assert(epd_service_cancelled(service));
    mock_thread.entry(mock_thread.parameter);
    app_service_process();
    assert(!services && !pins);

    /* Shell code keeps a UI-only service pinned until the call returns. */
    definition = (epd_background_t){.command = module_command, .ui_process = module_ui};
    service = app_service_start("books", &definition, &pins);
    assert(service);
    assert(app_service_command("books", 0, NULL) == 42);
    assert(app_service_command("books", 0, NULL) == -RT_ENOSYS);
    app_service_process();
    assert(!services && !pins);

    definition = (epd_background_t){module_worker, 4096, 22, true, NULL, module_ui, NULL};
    fail_thread_start = true;
    assert(!app_service_start("books", &definition, &pins));
    assert(!services && !pins && !subscriptions);
    puts("service: session cancellation, worker/UI/shell ordering, module pin and rollback passed");
    return 0;
}
