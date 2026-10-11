#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include "constants.h"
#include "intent.h"
#include "intent_int.h"
typedef int rt_err_t;
typedef int gui_app_msg_type_t;
typedef int lv_obj_t;
typedef int lv_image_dsc_t;
typedef struct { bool paused; void *data; } lv_timer_t;
enum { GUI_APP_MSG_ONSTART, GUI_APP_MSG_ONRESUME, GUI_APP_MSG_ONPAUSE, GUI_APP_MSG_ONSTOP,
       STORAGE_SD, LV_LABEL_LONG_DOT };
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#include "gallery_types.h"
static unsigned card_id = 7, requests, cancels, timers, stopped, freed, release_step;
static unsigned registered, queued;
static bool present = true, changing;
static int dummy;
static void *callback_memory;
static uint32_t queued_size;
static void (*queued_handler)(gui_app_msg_type_t, void *);
static bool storage_changing(void) { return changing; }
static bool storage_path_available(const char *path) { (void)path; return present; }
static const char *storage_path_root(const char *path)
{ return !strncmp(path, "/sdcard/", 8) ? STORAGE_SD_ROOT : STORAGE_FLASH_ROOT; }
static unsigned storage_card_session(void) { return card_id; }
static bool storage_session_valid(int volume, uint32_t session)
{ (void)volume; return present && session == card_id; }
static void storage_lock(void) {}
static void storage_unlock(void) {}
static int host_stat(const char *path, struct stat *st)
{ (void)path; memset(st, 0, sizeof(*st)); st->st_mode = _S_IFREG; return present ? 0 : -1; }
#define stat(path, st) host_stat(path, st)
#include "receive_under_test.h"
static const char *app_content_error(int error) { (void)error; return "error"; }
static bool gallery_start(void) { return true; }
static void gallery_stop(void) { ++stopped; }
static uint32_t gallery_submit(int kind, const char *path, unsigned first)
{ (void)kind; (void)path; (void)first; return ++requests; }
static void gallery_cancel_request(uint32_t serial) { (void)serial; ++cancels; }
static void gallery_result_free(gallery_result_t *result)
{ assert(release_step++ == 4); free(result); ++freed; }
static void lv_image_set_src(lv_obj_t *obj, const void *data)
{ (void)obj; assert(data == NULL && release_step++ == 0); }
static void lv_draw_wait_for_finish(void) { assert(release_step++ == 1); }
static void lv_image_cache_drop(void *key) { (void)key; assert(release_step++ == 2); }
static void lv_image_header_cache_drop(void *key) { (void)key; assert(release_step++ == 3); }
static lv_timer_t *lv_timer_create(void (*cb)(lv_timer_t *), unsigned period, void *data)
{
    (void)cb; (void)period;
    lv_timer_t *timer = calloc(1, sizeof(*timer));
    assert(timer);
    timer->data = data;
    ++timers;
    return timer;
}
static void lv_timer_pause(lv_timer_t *timer) { timer->paused = true; }
static void lv_timer_resume(lv_timer_t *timer) { timer->paused = false; }
static void lv_timer_delete(lv_timer_t *timer) { free(timer); --timers; }
static void *gui_app_this_page_memory(void) { return callback_memory; }
static void viewer_message(gui_app_msg_type_t message, void *parameter);
static int gui_app_create_page_for_app_ext(const char *id, const char *name,
    void (*handler)(gui_app_msg_type_t, void *), void *data, uint32_t size)
{
    assert(!strcmp(id, "gallery") && !strcmp(name, "picture") && data == NULL);
    queued_handler = handler;
    queued_size = size;
    ++queued;
    return RT_EOK;
}
static int gui_app_regist_msg_handler_ext(const char *id,
    void (*handler)(gui_app_msg_type_t, void *), void *data, uint32_t size)
{ (void)id; (void)handler; (void)data; (void)size; ++registered; return RT_EOK; }
static void visible(lv_obj_t *obj, bool show) { (void)obj; (void)show; }
static void browser_poll(lv_timer_t *timer) { (void)timer; }
static void viewer_poll(lv_timer_t *timer) { (void)timer; }
#define prepare(...) (&dummy)
#define epd_app_label(...) (&dummy)
#define epd_app_centered_label(...) (&dummy)
#define epd_app_button(...) (&dummy)
#define epd_app_header_refresh(...) ((void)0)
#define epd_app_text(...) ((void)0)
#define lv_label_set_long_mode(...) ((void)0)
#define lv_image_create(...) (&dummy)
#define browser_scan(page) ((void)(page))
#define viewer_request(page) ((page)->request = gallery_submit(GALLERY_IMAGE, (page)->path, 0))
#include "gallery_lifecycle_under_test.h"
void *content_test_malloc(size_t size) { return malloc(size); }
void content_test_free(void *memory) { free(memory); }
int gui_app_run_by_intent(intent_t intent) { (void)intent; assert(false); return -1; }
static void enter(const char *path)
{
    char session[9];
    snprintf(session, sizeof(session), "%x", card_id);
    intent_t intent = intent_init("gallery");
    assert(intent_set_string(intent, "path", path) == RT_EOK);
    assert(intent_set_string(intent, "s", session) == RT_EOK);
    assert(app_main(intent) == RT_EOK);
    intent_deinit(intent);
}
static browser_t *start_browser(void)
{
    browser_t *page = calloc(1, sizeof(*page));
    assert(page);
    callback_memory = page;
    browser_message(GUI_APP_MSG_ONSTART, NULL);
    browser_message(GUI_APP_MSG_ONRESUME, NULL);
    return page;
}
static viewer_t *start_viewer(void)
{
    assert(queued_handler == viewer_message && queued_size == sizeof(viewer_t));
    viewer_t *page = calloc(1, queued_size);
    assert(page);
    callback_memory = page;
    queued_handler(GUI_APP_MSG_ONSTART, NULL);
    queued_handler(GUI_APP_MSG_ONRESUME, NULL);
    page->displayed = calloc(1, sizeof(*page->displayed));
    assert(page->displayed);
    return page;
}
int main(void)
{
    enter("/sdcard/中文 空格/旧图片.jpg");
    browser_t *browser = start_browser();
    assert(queued == 1 && !launch_path[0]);
    browser_message(GUI_APP_MSG_ONPAUSE, NULL);
    viewer_t *viewer = start_viewer();
    assert(!strcmp(viewer->path, "/sdcard/中文 空格/旧图片.jpg"));
    unsigned before_cancel = cancels;
    /* An entry call must not release or modify the old viewer even if its
     * ONPAUSE/ONSTOP callbacks have not run yet. */
    enter("/sdcard/中文 空格/新图片.png");
    assert(registered == 2 && freed == 0 && cancels == before_cancel && timers == 2);
    assert(!strcmp(viewer->path, "/sdcard/中文 空格/旧图片.jpg"));
    assert(!strcmp(browser->selected, viewer->path));
    callback_memory = viewer;
    viewer_message(GUI_APP_MSG_ONPAUSE, NULL);
    assert(viewer->timer->paused && freed == 0);
    viewer_message(GUI_APP_MSG_ONSTOP, NULL);
    assert(freed == 1 && release_step == 5 && timers == 1 && !viewer->displayed);
    free(viewer);
    callback_memory = browser;
    browser_message(GUI_APP_MSG_ONSTOP, NULL);
    free(browser);
    assert(stopped == 1 && timers == 0);
    browser = start_browser();
    assert(queued == 2 && !strcmp(picture_path, "/sdcard/中文 空格/新图片.png"));
    /* Destroy parent storage before the deferred picture callback. The queued
     * request has NULL userdata and the picture reads its module-owned copy. */
    browser_message(GUI_APP_MSG_ONPAUSE, NULL);
    browser_message(GUI_APP_MSG_ONSTOP, NULL);
    free(browser);
    viewer = start_viewer();
    assert(!strcmp(viewer->path, "/sdcard/中文 空格/新图片.png"));
    viewer_message(GUI_APP_MSG_ONPAUSE, NULL);
    ++card_id;
    release_step = 0;
    unsigned before_request = requests;
    viewer_message(GUI_APP_MSG_ONRESUME, NULL);
    assert(!viewer->displayed && freed == 2 && release_step == 5 && requests == before_request);
    viewer_message(GUI_APP_MSG_ONSTOP, NULL);
    free(viewer);
    assert(timers == 0 && freed == 2 && stopped == 2);
    puts("gallery: actual entry/page callbacks, early entry, owned paths, background pause, image release order and removed-card resume passed");
    return 0;
}
