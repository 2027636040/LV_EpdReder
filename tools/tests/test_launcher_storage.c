#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define RT_EOK 0
#define NAV_IDLE (-1)
enum { UI_PAGE_HOME, UI_PAGE_FILES, UI_PAGE_STORAGE, UI_PAGE_APP_INSTALL,
       UI_PAGE_APP_UNINSTALL, UI_PAGE_APP_SETTINGS };
typedef struct { int id; bool foreground; void *group; } page_t;
static page_t page = { UI_PAGE_HOME, true, NULL }, *current = &page;
static int pending_page;
static uint32_t mock_revision = 1, mock_request, request_during_cleanup;
static bool display_done = true, cleanup_done, timed_out, changing;
static unsigned pauses, catalog_changes, recovery_changes, cleanup_calls;
static unsigned ack_calls, accepted_acks, files_updates, capacity_updates, list_updates;
static uint32_t last_ack;

static void app_card_process(void) {}
static bool lv_refreshing_done(void) { return display_done; }
static uint32_t storage_release_request_id(void) { return mock_request; }
static uint32_t storage_revision(void) { return mock_revision; }
static bool storage_changing(void) { return changing; }
static bool storage_release_timed_out(void) { return timed_out; }
static void home_pause(void) { ++pauses; }
static void app_catalog_changed(void) { ++catalog_changes; }
static void app_installer_storage_changed(void) { ++recovery_changes; }
static void files_storage_changed(void) { ++files_updates; }
static void storage_page_resume(void) { ++capacity_updates; }
static void app_list_storage_changed(void) { ++list_updates; }
static void *lv_group_get_focused(void *group) { return group; }
static bool focus_eligible(void *object) { (void)object; return true; }
static void focus_restore(unsigned index) { (void)index; }
static bool app_catalog_release_storage(void)
{
    ++cleanup_calls;
    if (request_during_cleanup)
        mock_request = request_during_cleanup;
    return cleanup_done;
}
static int storage_release_complete(uint32_t id)
{
    ++ack_calls;
    last_ack = id;
    if (id != mock_request) return -1;
    ++accepted_acks;
    return RT_EOK;
}

#include "launcher_storage_under_test.h"

int main(void)
{
    /* Initial/no-card and later READY revisions use the existing UI recovery path. */
    assert(launcher_storage_process());
    assert(recovery_changes == 1 && catalog_changes == 1);
    assert(launcher_storage_process() && recovery_changes == 1);
    page.id = UI_PAGE_FILES;
    ++mock_revision;
    assert(launcher_storage_process() && recovery_changes == 2 && files_updates == 1);
    assert(launcher_storage_process() && files_updates == 1);
    page.id = UI_PAGE_STORAGE;
    ++mock_revision;
    assert(launcher_storage_process() && capacity_updates == 1);
    page.id = UI_PAGE_APP_INSTALL;
    ++mock_revision;
    assert(launcher_storage_process() && list_updates == 1);
    page.id = UI_PAGE_HOME;
    pending_page = 3;
    mock_request = 10;
    display_done = false;
    assert(!launcher_storage_process() && !cleanup_calls && !pauses && !ack_calls);
    display_done = true;
    assert(!launcher_storage_process() && cleanup_calls == 1 && pauses == 1);
    assert(pending_page == NAV_IDLE && !ack_calls);
    assert(!launcher_storage_process() && cleanup_calls == 2 && pauses == 1);
    timed_out = true;
    assert(launcher_storage_process() && !ack_calls);
    cleanup_done = true;
    assert(!launcher_storage_process() && accepted_acks == 1 && last_ack == 10);
    unsigned completed_cleanup_calls = cleanup_calls;
    assert(!launcher_storage_process() && ack_calls == 1);
    assert(cleanup_calls == completed_cleanup_calls);
    mock_request = 0;
    changing = true;
    ++mock_revision;
    assert(!launcher_storage_process() && recovery_changes == 4);
    changing = false;
    assert(launcher_storage_process() && recovery_changes == 5);
    mock_request = 11;
    request_during_cleanup = 12;
    assert(!launcher_storage_process() && last_ack == 11 && accepted_acks == 1);
    request_during_cleanup = 0;
    assert(!launcher_storage_process() && last_ack == 12 && accepted_acks == 2);
    assert(pauses == 3);
    mock_request = 0;
    assert(launcher_storage_process() && recovery_changes == 5);
    puts("launcher storage: revision delivery, release ordering, single ACK and stale ID passed");
    return 0;
}
