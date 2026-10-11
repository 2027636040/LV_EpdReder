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
typedef int lv_group_t;
typedef int lv_font_t;
enum { GUI_APP_MSG_ONSTART, GUI_APP_MSG_ONRESUME, GUI_APP_MSG_ONPAUSE, GUI_APP_MSG_ONSTOP,
       STORAGE_SD, LV_OPA_COVER, UI_READER_TOP = 100 };
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#include "books_page_types.h"

static books_page_t *root_page, *reader_page, *callback_page;
static unsigned opens, closes, entries, pauses, stops, popups, requests;
static unsigned card_id = 5;
static bool present = true, changing, display_ready = true, init_ok = true, defer_registration;
static int dummy;
static char imported[STORAGE_PATH_MAX];
bool reader_session;
static const int input_ops = 0;
static void page_message(gui_app_msg_type_t message, void *parameter);
static void activate_page(books_page_t *page);
static bool books_initialize(void) { return init_ok; }
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
static bool lv_refreshing_done(void) { return display_ready; }
static void popup_open(const char *text, const char *detail) { (void)text; (void)detail; ++popups; }
static void ui_reader_close(void) { assert(reader_page == NULL); ++closes; }
static bool ui_bookshelf_include_path(const char *path, unsigned *index)
{ assert(!reader_session); snprintf(imported, sizeof(imported), "%s", path); *index = 32; return true; }
static bool ui_reader_open(unsigned index) { assert(index == 32); ++opens; return true; }
static int gui_app_is_page_present(char *name) { return !strcmp(name, "reader") && reader_page != NULL; }
static int gui_app_goback_to_page(const char *name) { (void)name; activate_page(reader_page); return RT_EOK; }
static bool epd_app_open_text_settings(void) { return true; }
static void gui_app_exec_now(void) {}
static void clicked(void) {}
static void gui_app_close_anim(void) {}
static void *gui_app_this_page_memory(void) { return callback_page; }
static void *gui_app_this_page_userdata(void) { return (void *)(uintptr_t)callback_page->id; }
static lv_obj_t *lv_screen_active(void) { return &dummy; }
static lv_group_t *epd_app_input_group(lv_obj_t *screen) { (void)screen; return &dummy; }
static void epd_input_set_ops(lv_group_t *group, const int *ops) { (void)group; (void)ops; }
#define epd_obj_init(...) ((void)0)
#define lv_obj_set_size(...) ((void)0)
#define lv_obj_set_style_bg_color(...) ((void)0)
#define lv_obj_set_style_bg_opa(...) ((void)0)
#define lv_obj_set_style_text_font(...) ((void)0)
#define lv_obj_set_y(...) ((void)0)
#define epd_app_header(...) ((void)0)
#define epd_app_header_refresh(...) ((void)0)
static void reader_start(lv_obj_t *screen) { (void)screen; }
static void bookshelf_start(lv_obj_t *screen) { (void)screen; }
static void reader_foreground(void) {}
static void bookshelf_resume(void) {}
static void reader_pause(void) { ++pauses; }
static void bookshelf_pause(void) {}
static void reader_stop(void) { ++stops; }
static unsigned focus_index(void) { return 0; }
static void focus_restore(unsigned index) { (void)index; }
static lv_obj_t *epd_app_back_button_cb(lv_obj_t *screen, void (*cb)(void), void *data)
{ (void)screen; (void)cb; (void)data; return &dummy; }
static int gui_app_create_page_for_app_ext(const char *id, const char *name,
    void (*handler)(gui_app_msg_type_t, void *), void *data, uint32_t size)
{
    (void)id; (void)name; (void)handler; (void)data;
    assert(size == sizeof(books_page_t) && !reader_page);
    reader_page = calloc(1, size);
    assert(reader_page);
    reader_page->id = UI_PAGE_READER;
    activate_page(reader_page);
    return RT_EOK;
}
static int gui_app_regist_msg_handler_ext(const char *id, void (*handler)(gui_app_msg_type_t, void *),
                                        void *data, uint32_t size)
{
    (void)id; (void)handler; (void)data;
    if (defer_registration) return RT_EOK;
    assert(!root_page && !reader_page && size == sizeof(books_page_t));
    root_page = calloc(1, size);
    assert(root_page);
    root_page->id = UI_PAGE_BOOKSHELF;
    activate_page(root_page);
    ++entries;
    return RT_EOK;
}
#include "books_lifecycle_under_test.h"

static void activate_page(books_page_t *page)
{
    if (current && current->foreground)
    { callback_page = current; page_message(GUI_APP_MSG_ONPAUSE, NULL); }
    callback_page = page;
    if (!page->screen) page_message(GUI_APP_MSG_ONSTART, NULL);
    page_message(GUI_APP_MSG_ONRESUME, NULL);
}
static void stop_pages(void)
{
    if (current && current->foreground)
    { callback_page = current; page_message(GUI_APP_MSG_ONPAUSE, NULL); }
    if (reader_page)
    { callback_page = reader_page; page_message(GUI_APP_MSG_ONSTOP, NULL); free(reader_page); reader_page = NULL; }
    if (root_page)
    { callback_page = root_page; page_message(GUI_APP_MSG_ONSTOP, NULL); free(root_page); root_page = NULL; }
    assert(current == NULL && live_pages == 0);
}

/* Execute the SDK's actual RUN_APP switch body, with page-list operations
 * driving the production books callback above. */
typedef struct { int entry; } mailbox_t;
typedef struct { int count; } list_t;
typedef struct { _intent param; list_t node, page_list; } gui_runing_app_t;
typedef struct { union { _intent intnt; } content; unsigned tick; } gui_app_msg_t;
static gui_runing_app_t running;
static list_t running_app_list = {2};
static bool running_live, retry_pending;
static gui_app_msg_t retried;
enum { app_st_destoryed };
#define app_sche_d(...) ((void)0)
#define app_sche_i(...) ((void)0)
#define app_sche_assert(...) assert(false)
static gui_runing_app_t *get_runing_app_handler(const char *id)
{ assert(!strcmp(id, "books")); return running_live ? &running : NULL; }
static bool rt_list_isempty(list_t *list) { (void)list; return root_page == NULL; }
static int rt_list_len(list_t *list) { return list->count; }
static void app_resume(list_t *node) { (void)node; activate_page(reader_page ? reader_page : root_page); }
static void app_exit(list_t *node) { (void)node; stop_pages(); running_live = false; }
static void app_stop_running(void) { app_exit(&running.node); }
static void app_run(mailbox_t *mbx, _intent *intent, unsigned tick)
{ (void)mbx; retried.content.intnt = *intent; retried.tick = tick; retry_pending = true; }
static gui_runing_app_t *get_suspend_or_destoryed_app_handler(const char *id) { (void)id; return NULL; }
static void app_set_target_state(gui_runing_app_t *app, int state) { (void)app; (void)state; }
static void do_destory_apps(void) {}
static void app_launch_new(const char *id, intent_t intent)
{
    (void)id;
    running.param = *(_intent *)intent;
    running_live = true;
    assert(app_main(&running.param) == RT_EOK);
}
#include "sdk_dispatch_under_test.h"
void *content_test_malloc(size_t size) { return malloc(size); }
void content_test_free(void *memory) { free(memory); }
int gui_app_run_by_intent(intent_t intent)
{
    gui_app_msg_t msg = {0};
    mailbox_t mbx = {0};
    msg.content.intnt = *(_intent *)intent;
    dispatch_content(&msg, &mbx);
    while (retry_pending) { retry_pending = false; dispatch_content(&retried, &mbx); }
    return RT_EOK;
}
static void open_content(const char *path)
{
    intent_t intent = intent_init("books");
    char serial[9], session[9];
    snprintf(serial, sizeof(serial), "%x", ++requests);
    snprintf(session, sizeof(session), "%x", card_id);
    assert(intent_set_string(intent, "path", path) == RT_EOK);
    assert(intent_set_string(intent, "s", session) == RT_EOK);
    assert(intent_set_string(intent, "r", serial) == RT_EOK);
    assert(intent_runapp(intent) == RT_EOK);
    intent_deinit(intent);
}

int main(void)
{
    intent_t ordinary = intent_init("books");
    assert(intent_runapp(ordinary) == RT_EOK);
    intent_deinit(ordinary);
    assert(entries == 1 && current == root_page && !reader_page);
    open_content("/sdcard/目录/第一本.txt");
    assert(entries == 2 && current == root_page && live_pages == 1);
    display_ready = false;
    content_process();
    assert(opens == 0);
    display_ready = true;
    content_process();
    assert(opens == 1 && current == reader_page && reader_session && live_pages == 2);
    assert(!strcmp(imported, "/sdcard/目录/第一本.txt"));
    callback_page = current;
    page_message(GUI_APP_MSG_ONPAUSE, NULL);
    assert(!current);
    open_content("/sdcard/目录/第二本.txt");
    assert(entries == 3 && stops == 1 && live_pages == 1);
    content_process();
    assert(opens == 2 && current == reader_page);
    assert(!strcmp(imported, "/sdcard/目录/第二本.txt"));
    running_app_list.count = 1;
    open_content("/sdcard/目录/第二本.txt");
    content_process();
    assert(entries == 4 && stops == 2 && opens == 3);
    open_content("/sdcard/目录/第三本.txt");
    ++card_id;
    content_process();
    assert(opens == 3 && popups == 1 && !reader_session);
    stop_pages();
    /* Background service/module data stay alive without any page. */
    running_live = false;
    open_content("/sdcard/目录/第四本.txt");
    content_process();
    assert(current == reader_page && opens == 4);
    stop_pages();
    content_process();
    assert(!current && !live_pages && pauses >= 3 && closes >= 4);
    /* Deliberately call entry before old callbacks, independently of the
     * dispatcher replacement's normal ordering. Neither a live reader nor
     * an old-generation root may consume the new request. */
    running_live = false;
    open_content("/sdcard/旧目录/阅读中.txt");
    content_process();
    assert(reader_page && current == reader_page);
    unsigned before_close = closes, before_open = opens;
    ordinary = intent_init("books");
    char session[9];
    snprintf(session, sizeof(session), "%x", card_id);
    assert(intent_set_string(ordinary, "path", "/sdcard/新目录/替换文件.txt") == RT_EOK);
    assert(intent_set_string(ordinary, "s", session) == RT_EOK);
    defer_registration = true;
    assert(app_main(ordinary) == RT_EOK);
    intent_deinit(ordinary);
    content_process();
    assert(closes == before_close && opens == before_open && current == reader_page);
    activate_page(root_page);
    content_process();
    assert(closes == before_close && opens == before_open);
    callback_page = reader_page;
    page_message(GUI_APP_MSG_ONSTOP, NULL);
    free(reader_page);
    reader_page = NULL;
    assert(live_pages == 1 && current == root_page);
    content_process();
    assert(closes == before_close && opens == before_open);
    stop_pages();
    defer_registration = false;
    assert(gui_app_regist_msg_handler_ext("books", page_message, NULL, sizeof(books_page_t)) == RT_EOK);
    content_process();
    assert(closes == before_close + 1 && opens == before_open + 1);
    assert(!strcmp(imported, "/sdcard/新目录/替换文件.txt"));
    stop_pages();
    puts("lifecycle: SDK RUN_APP branch, running root/reader, background, repeated file, display wait, card removal, early entry and teardown guards passed");
    return 0;
}
