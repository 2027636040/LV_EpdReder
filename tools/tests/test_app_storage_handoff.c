#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RT_EOK 0
#define GUI_APP_ID_MAX_LEN 16
#define EPD_PACKAGE_DIRECTORY "/sdcard/app-packages"
typedef int rt_base_t;
typedef uint32_t rt_tick_t;
typedef void *rt_thread_t;
typedef void *rt_list_t;
typedef enum { STORAGE_FLASH, STORAGE_SD, STORAGE_COUNT } storage_volume_t;
typedef struct { bool resources_only; } epd_app_entry_t;
enum { APP_JOB_RUNNING, APP_JOB_FAILED };
static struct { int state; uint32_t ticket; char message[64]; } result;
static bool recovery_ready[STORAGE_COUNT], needs_prepare, prepare_ok, exit_sent, prepared_resources;
static uint32_t recovery_session, job_session;
static rt_tick_t prepare_started;
static char job_id[GUI_APP_ID_MAX_LEN];
static bool remove_job;
static storage_volume_t job_volume;
static uint32_t background_revision;
static uint32_t mock_session = 1;
static bool available = true, changing, releasing, busy;
static bool module_refs, background_running, pages_done;
static bool bound;
static unsigned locate_calls, thread_starts, stopped, process_calls;
static storage_volume_t bound_volume;
static uint32_t bound_session;
typedef struct { char id[16]; } gui_runing_app_t;
static gui_runing_app_t apps[] = {{"Main"}, {"books"}, {"weather"}};
static unsigned app_count = 3, iterator;

static rt_base_t rt_hw_interrupt_disable(void) { return 0; }
static void rt_hw_interrupt_enable(rt_base_t level) { (void)level; }
static bool storage_available(storage_volume_t volume) { return volume == STORAGE_FLASH || available; }
static uint32_t storage_card_session(void) { return mock_session; }
static bool storage_session_valid(storage_volume_t volume, uint32_t value)
{ return volume == STORAGE_FLASH || (available && value == mock_session); }
static bool storage_release_requested(void) { return releasing; }
static bool storage_changing(void) { return changing; }
static bool app_installer_busy(void) { return busy; }
static void storage_lock(void) {}
static void storage_unlock(void) {}
static bool valid_id(const char *id) { return *id != 0; }
static bool storage_app_locate(const char *id, storage_volume_t *volume)
{ (void)id; ++locate_calls; *volume = STORAGE_FLASH; return true; }
static const char *storage_app_directory(storage_volume_t volume)
{ (void)volume; return "/flash/apps"; }
static bool app_package_read(const char *directory, const char *id, epd_app_entry_t *entry)
{ (void)directory; (void)id; entry->resources_only = false; return true; }
static void worker(void *argument) { (void)argument; }
static rt_thread_t rt_thread_create(const char *name, void (*entry)(void *), void *argument,
                                   int stack, int priority, int slice)
{ (void)name; (void)entry; (void)argument; (void)stack; (void)priority; (void)slice; return &result; }
static int rt_thread_startup(rt_thread_t thread) { (void)thread; ++thread_starts; return RT_EOK; }
static void rt_thread_delete(rt_thread_t thread) { (void)thread; }
static rt_tick_t rt_tick_get(void) { return 1; }
static bool app_module_storage(const char *id, storage_volume_t *volume, uint32_t *value)
{ (void)id; *volume = bound_volume; *value = bound_session; return bound; }
static void app_installer_process(void) { ++process_calls; }
static void app_service_stop_modules(void) { ++stopped; }
static void app_service_process(void) {}
static bool app_service_modules_running(void) { return background_running; }
static bool app_modules_present(void) { return module_refs; }
static bool builtin_id(const char *id) { return !strcmp(id, "Main"); }
static gui_runing_app_t *gui_app_trav(rt_list_t **cursor)
{
    if (!*cursor) iterator = 0;
    *cursor = (rt_list_t *)&iterator;
    return iterator < app_count ? &apps[iterator++] : NULL;
}
static void gui_app_exit(const char *id) { assert(!builtin_id(id)); }
static void gui_app_exec_now(void) { if (pages_done) app_count = 1; }

/* MSVC does not follow the metadata-success boolean through the early return. */
#pragma warning(push)
#pragma warning(disable: 4701)
#include "handoff_under_test.h"
#pragma warning(pop)

int main(void)
{
    volume_ready_set(STORAGE_FLASH, true);
    volume_ready_set(STORAGE_SD, true);
    assert(app_installer_volume_ready(STORAGE_SD));
    ++mock_session;
    assert(!app_installer_volume_ready(STORAGE_SD));
    assert(app_installer_volume_ready(STORAGE_FLASH));
    volume_ready_set(STORAGE_SD, true);
    assert(app_installer_volume_ready(STORAGE_SD));
    available = false;
    assert(!app_installer_volume_ready(STORAGE_SD));
    uint32_t ticket = 0;
    releasing = true; changing = false; /* Exit timeout is not permission to start another job. */
    assert(!app_installer_start("books", true, STORAGE_FLASH, &ticket));
    assert(!thread_starts);
    releasing = false;
    assert(app_installer_start("books", true, STORAGE_FLASH, &ticket));
    assert(thread_starts == 1 && ticket == 1);

    bound = true; bound_volume = STORAGE_SD; bound_session = mock_session;
    storage_volume_t volume;
    unsigned lookups = locate_calls;
    assert(!app_location("books", &volume) && lookups == locate_calls);
    available = true; ++mock_session;
    assert(!app_location("books", &volume) && lookups == locate_calls);
    bound = false;
    assert(app_location("books", &volume) && volume == STORAGE_FLASH);

    busy = background_running = module_refs = true;
    assert(!app_catalog_release_storage() && stopped == 1 && process_calls == 1);
    pages_done = true;
    assert(!app_catalog_release_storage());
    background_running = false;
    assert(!app_catalog_release_storage());
    module_refs = false;
    assert(!app_catalog_release_storage());
    busy = false;
    assert(app_catalog_release_storage() && background_revision == UINT32_MAX);
    puts("handoff: all holders required, session-bound recovery, no fallback and timeout isolation passed");
    return 0;
}
