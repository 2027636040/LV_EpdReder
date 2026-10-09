#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

enum { STORAGE_SD, LV_OBJ_FLAG_HIDDEN, LV_OPA_TRANSP, LV_OBJ_FLAG_CLICKABLE, LV_STATE_DISABLED };
typedef struct { unsigned value; } lv_obj_t;
typedef struct { uintptr_t value; } lv_event_t;
typedef struct { bool resources_only; } epd_app_entry_t;
typedef int DIR;
struct dirent { char d_name[16]; };
#define STORAGE_SD_APPS "/sdcard/.epd/apps"
static lv_obj_t objects[16];
static unsigned object_count, prompts, dismissals, enabled_calls, recovery_calls, catalog_changes;
static uint32_t mock_session = 1, enabled_session;
static bool available = true, busy, recovered, has_package = true, remove_on_scan, display_done = true;
static DIR directory;
static struct dirent mock_entry = {"books"};
static bool emitted;

static bool storage_session_valid(int volume, uint32_t session)
{ (void)volume; return available && session == mock_session; }
static bool storage_available(int volume) { (void)volume; return available; }
static bool storage_apps_enabled(int volume) { return storage_session_valid(volume, enabled_session); }
static bool storage_card_enable_apps(uint32_t session)
{ if (!storage_session_valid(STORAGE_SD, session)) return false; enabled_session = session; ++enabled_calls; return true; }
static uint32_t storage_card_session(void) { return mock_session; }
static bool storage_changing(void) { return false; }
static void storage_lock(void) {}
static void storage_unlock(void) {}
static DIR *opendir(const char *path) { (void)path; emitted = false; return &directory; }
static struct dirent *readdir(DIR *dir)
{ (void)dir; if (remove_on_scan) available = false; if (!has_package || emitted) return NULL; emitted = true; return &mock_entry; }
static void closedir(DIR *dir) { (void)dir; }
static bool app_package_read(const char *path, const char *id, epd_app_entry_t *item)
{ (void)path; (void)id; item->resources_only = false; return true; }
static uint32_t app_catalog_revision(void) { return 1; }
static void app_catalog_changed(void) { ++catalog_changes; }
static bool app_installer_busy(void) { return busy; }
static bool app_installer_volume_ready(int volume) { (void)volume; return recovered; }
static void app_installer_storage_changed(void) { ++recovery_calls; recovered = false; }
static void app_installer_process(void) { busy = true; }
static bool epd_app_refresh_done(void) { return display_done; }
static void epd_input_clear(void) {}
static void *lv_event_get_user_data(lv_event_t *event) { return (void *)event->value; }
static void lv_obj_delete(lv_obj_t *obj) { (void)obj; ++dismissals; }
static lv_obj_t *lv_obj_get_parent(lv_obj_t *obj) { return obj; }
static unsigned lv_obj_get_child_count(lv_obj_t *obj) { (void)obj; return 1; }
static lv_obj_t *lv_obj_get_child(lv_obj_t *obj, unsigned child) { (void)child; return obj; }
static void lv_obj_add_flag(lv_obj_t *obj, int flag) { (void)obj; (void)flag; }
static void lv_obj_remove_flag(lv_obj_t *obj, int flag) { (void)obj; (void)flag; }
static void lv_obj_add_state(lv_obj_t *obj, int flag) { (void)obj; (void)flag; }
static lv_obj_t *lv_layer_top(void) { return NULL; }
static void lv_obj_set_style_bg_opa(lv_obj_t *obj, int value, int part) { (void)obj; (void)value; (void)part; }
static void lv_obj_set_style_border_width(lv_obj_t *obj, int value, int part) { (void)obj; (void)value; (void)part; }
static void lv_obj_set_style_border_color(lv_obj_t *obj, int value, int part) { (void)obj; (void)value; (void)part; }
static int lv_color_black(void) { return 0; }
static void epd_app_input_group(lv_obj_t *obj) { (void)obj; }
static void epd_app_input_back(lv_obj_t *obj, lv_obj_t *back) { (void)obj; (void)back; }
static void lv_group_focus_obj(lv_obj_t *obj) { (void)obj; }
static void epd_app_text(lv_obj_t *obj, const char *text) { (void)obj; (void)text; }
static const void *epd_app_font(void) { return NULL; }
static lv_obj_t *epd_app_panel(lv_obj_t *parent, int x, int y, int w, int h)
{
    (void)x; (void)y; (void)w; (void)h;
    if (!parent) { object_count = 0; ++prompts; }
    return &objects[object_count++];
}
static lv_obj_t *epd_app_centered_label(lv_obj_t *parent, const char *text,
                                      int x, int y, int w, const void *font)
{ (void)parent; (void)text; (void)x; (void)y; (void)w; (void)font; return &objects[object_count++]; }
static lv_obj_t *epd_app_button(lv_obj_t *parent, int x, int y, int w, int h,
                              const char *text, void (*callback)(lv_event_t *), void *data)
{ (void)parent; (void)x; (void)y; (void)w; (void)h; (void)text; (void)callback; (void)data; return &objects[object_count++]; }

#include "card_under_test.h"

int main(void)
{
    lv_event_t reject = {0}, accept = {1};
    display_done = false;
    app_card_process();
    assert(!prompts);
    display_done = true;
    app_card_process();
    assert(prompts == 1 && prompt_session == 1 && modal);
    answer(&reject);
    assert(!modal && available && !enabled_calls);
    app_card_process();
    assert(prompts == 1);
    app_card_request();
    app_card_process();
    assert(prompts == 2);
    ++mock_session;
    answer(&accept);
    assert(!modal && !enabled_calls && !recovery_calls);
    app_card_process();
    assert(prompts == 3 && prompt_session == 2);
    answer(&accept);
    assert(enabled_calls == 1 && recovery_calls == 1 && loading && busy);
    app_card_process();
    assert(modal);
    busy = false; recovered = true;
    app_card_process();
    assert(!modal && catalog_changes == 1);
    app_card_process();
    assert(prompts == 3);

    ++mock_session;
    remove_on_scan = true;
    app_card_process();
    assert(!modal && prompts == 3);
    remove_on_scan = false; available = true; ++mock_session;
    app_card_process();
    assert(modal && prompts == 4);
    available = false;
    app_card_process();
    assert(!modal && dismissals == 4);
    puts("card: cancel, manual retry, stale accept, removal during scan and recovery gate passed");
    return 0;
}
