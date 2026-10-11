/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "../weread_ui.c"

/* Real application callbacks and LVGL objects; framework/worker boundaries are
 * deterministic substitutes. app_fwk passes app_id, not user_data, to callbacks. */
typedef struct {
    gui_page_msg_cb_t callback;
    void *userdata;
    page_t *memory;
    lv_obj_t *screen;
} test_page_t;
static test_page_t pages[3], queued, *current;
static unsigned depth, live_allocations, submissions, stops, scenarios;
static uint32_t serial;
static bool deferred;
static int fail_after = -1;
static weread_result_t *published;
static const weread_result_t *submitted_catalog;
static size_t submitted_index;
static char submitted_book[64], submitted_title[64];

void *epd_app_alloc(size_t size, int region)
{
    (void)region;
    if (fail_after == 0) { fail_after = -1; return NULL; }
    if (fail_after > 0) --fail_after;
    void *memory = calloc(1, size);
    assert(memory); ++live_allocations;
    return memory;
}
void epd_app_free(void *memory)
{ if (memory) { assert(live_allocations); --live_allocations; free(memory); } }
char *weread_string(const char *text)
{
    if (!text) text = "";
    char *copy = epd_app_alloc(strlen(text) + 1, EPD_APP_PSRAM);
    if (copy) strcpy(copy, text);
    return copy;
}
void weread_result_retain(weread_result_t *result)
{ assert(result && result->references); ++result->references; }
void weread_result_free(weread_result_t *result)
{
    if (!result) return;
    assert(result->references);
    if (--result->references) return;
    /* Fixtures own only the snapshot; strings and arrays are immutable literals. */
    epd_app_free(result);
}
weread_result_t *weread_result_get(void)
{ if (published) weread_result_retain(published); return published; }
bool weread_start(void) { return true; }
bool weread_stopping(void) { return false; }
void weread_stop(void) { ++stops; }
bool weread_unavailable(void) { return false; }
bool weread_failed(uint32_t request) { (void)request; return false; }
void weread_cancel(uint32_t request) { (void)request; }
void weread_wake(void) {}
uint32_t weread_submit(weread_action_t action, const char *book, const char *title,
                       const char *chapter, const char *chapter_title)
{
    (void)chapter; (void)chapter_title;
    if (action == WEREAD_CATALOG || action == WEREAD_CATALOG_RESTORE)
    {
        assert(book && title);
        snprintf(submitted_book, sizeof(submitted_book), "%s", book);
        snprintf(submitted_title, sizeof(submitted_title), "%s", title);
    }
    ++submissions; return ++serial;
}
uint32_t weread_submit_chapter(const weread_result_t *catalog, size_t index)
{
    assert(catalog && index < catalog->chapter_count);
    submitted_catalog = catalog; submitted_index = index;
    ++submissions; return ++serial;
}
uint32_t weread_submit_code(uint32_t generation, const char *code)
{ (void)generation; (void)code; return ++serial; }
bool weread_save_position(const weread_result_t *content, const reader_position_t *position)
{ (void)content; (void)position; return true; }
uint32_t weread_covers_request(const weread_result_t *result, size_t first, size_t count)
{ (void)result; (void)first; (void)count; return 0; }
void weread_covers_cancel(void) {}
weread_cover_t *weread_covers_take(void) { return NULL; }
void weread_covers_free(weread_cover_t *cover) { assert(!cover); }
bool weread_store_path(char *path, storage_app_area_t area, const char *account,
                       const char *book, const char *name)
{ (void)path; (void)area; (void)account; (void)book; (void)name; return false; }
int weread_resource_open_ex(const weread_result_t *content, const char *base,
    const char *reference, book_stream_t *stream, bool (*cancel)(void *), void *user)
{ (void)content; (void)base; (void)reference; (void)stream; (void)cancel; (void)user; return BOOK_IO_ERROR; }

const reader_view_t *epd_reader_get(void) { static reader_view_t view; return &view; }
bool epd_reader_open(const reader_options_t *options)
{ options->release_user(options->user); return false; }
void epd_reader_close(void) {}
void epd_reader_collect(void) {}
bool epd_reader_save(void) { return true; }
bool epd_reader_reflow(void) { return true; }
bool epd_reader_settings_changed(void) { return false; }
bool epd_reader_waiting(void) { return false; }
void epd_reader_seek(unsigned page) { (void)page; }
bool epd_reader_bind(lv_obj_t *body) { (void)body; return true; }
reader_work_t epd_reader_step(uint32_t budget, bool foreground, bool commit)
{ (void)budget; (void)foreground; (void)commit; return READER_WORK_IDLE; }

void epd_app_object_init(lv_obj_t *object) { assert(object); }
const lv_font_t *epd_app_font(void) { return LV_FONT_DEFAULT; }
const lv_font_t *epd_app_font_role(epd_font_role_t role) { (void)role; return LV_FONT_DEFAULT; }
static void group_delete(lv_event_t *event) { lv_group_delete(lv_event_get_user_data(event)); }
lv_group_t *epd_app_input_group(lv_obj_t *owner)
{
    lv_group_t *group = lv_group_create();
    lv_obj_add_event_cb(owner, group_delete, LV_EVENT_DELETE, group);
    return group;
}
void epd_app_input_back(lv_obj_t *owner, lv_obj_t *button) { (void)owner; (void)button; }
void epd_app_header(lv_obj_t *screen) { (void)screen; }
void epd_app_header_refresh(lv_obj_t *screen) { (void)screen; }
bool epd_app_refresh_done(void) { return true; }
bool epd_app_open_text_settings(void) { return true; }
void epd_service_ui_request(epd_service_t *service, unsigned delay) { (void)service; (void)delay; }
void epd_app_status(epd_app_status_t *status) { memset(status, 0, sizeof(*status)); }
lv_obj_t *epd_app_panel(lv_obj_t *parent, int x, int y, int width, int height)
{
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_set_pos(object, x, y); lv_obj_set_size(object, width, height);
    return object;
}
lv_obj_t *epd_app_label(lv_obj_t *parent, const char *text, int x, int y, int width, const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y); lv_obj_set_width(label, width);
    lv_obj_set_style_text_font(label, font, 0); lv_label_set_text(label, text);
    return label;
}
lv_obj_t *epd_app_centered_label(lv_obj_t *parent, const char *text, int x, int y, int width, const lv_font_t *font)
{ return epd_app_label(parent, text, x, y, width, font); }
lv_obj_t *epd_app_button(lv_obj_t *parent, int x, int y, int width, int height,
                        const char *text, lv_event_cb_t callback, void *data)
{
    lv_obj_t *button = epd_app_panel(parent, x, y, width, height);
    if (text) epd_app_label(button, text, 0, 0, width, LV_FONT_DEFAULT);
    if (callback) lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, data);
    return button;
}
lv_obj_t *epd_app_back_button(lv_obj_t *screen)
{ return epd_app_button(screen, 0, 0, 60, 40, "Back", NULL, NULL); }
void epd_app_text(lv_obj_t *label, const char *text) { lv_label_set_text(label, text); }

void *gui_app_this_page_memory(void) { assert(current); return current->memory; }
void *gui_app_this_page_userdata(void) { assert(current); return current->userdata; }
void gui_app_close_anim(void) {}
static void dispatch(test_page_t *page, gui_app_msg_type_t message)
{
    current = page;
    page->callback(message, (void *)"weread");
    current = NULL;
}
int gui_app_create_page_for_app_ext(const char *app, const char *name,
    gui_page_msg_cb_t callback, void *userdata, uint32_t size)
{
    assert(!strcmp(app, "weread") && name && size == sizeof(page_t));
    assert(!queued.callback && depth < 3);
    queued = (test_page_t){.callback = callback, .userdata = userdata};
    return 0;
}
int gui_app_regist_msg_handler_ext(const char *app, gui_page_msg_cb_t callback, void *userdata, uint32_t size)
{ return gui_app_create_page_for_app_ext(app, "root", callback, userdata, size); }
void gui_app_exec_now(void)
{
    if (deferred || !queued.callback) return;
    if (depth) dispatch(&pages[depth - 1], GUI_APP_MSG_ONPAUSE);
    test_page_t *page = &pages[depth++];
    *page = queued; queued = (test_page_t){0};
    page->memory = calloc(1, sizeof(page_t)); assert(page->memory);
    page->screen = lv_obj_create(NULL); lv_screen_load(page->screen);
    dispatch(page, GUI_APP_MSG_ONSTART); dispatch(page, GUI_APP_MSG_ONRESUME);
}
void gui_app_goback(void)
{
    assert(depth > 1);
    test_page_t *page = &pages[--depth];
    dispatch(page, GUI_APP_MSG_ONPAUSE);
    lv_screen_load(pages[depth - 1].screen);
    dispatch(&pages[depth - 1], GUI_APP_MSG_ONRESUME);
    dispatch(page, GUI_APP_MSG_ONSTOP);
    lv_obj_delete(page->screen); free(page->memory); *page = (test_page_t){0};
}

static void publish_list(page_t *page)
{
    static wr_book_t books[] = {{.id = "book-A", .title = "Title A"}, {.id = "book-B", .title = "Title B"}};
    static wr_chapter_t chapters[] = {{.uid = "c1", .title = "Chapter 1"}, {.uid = "c2", .title = "Chapter 2"}};
    weread_result_t *result = epd_app_alloc(sizeof(*result), EPD_APP_PSRAM);
    *result = (weread_result_t){.references = 1, .generation = page->request,
        .revision = ++serial, .result = WR_OK, .account = "account",
        .action = page->kind == PAGE_SHELF ? WEREAD_SHELF : WEREAD_CATALOG,
        .book = "book-B", .title = "Title B", .last_chapter = "c1"};
    if (page->kind == PAGE_SHELF) { result->books = books; result->book_count = 2; }
    else { result->chapters = chapters; result->chapter_count = 2; }
    published = result; poll(page->timer); published = NULL;
    weread_result_free(result); assert(page->result->references == 1);
}
static void click(lv_obj_t *button) { lv_obj_send_event(button, LV_EVENT_CLICKED, NULL); }

int main(void)
{
    lv_init();
    lv_display_t *display = lv_display_create(684, 1216);
    lv_obj_t *empty = lv_screen_active();
    assert(app_main(NULL) == 0); gui_app_exec_now();
    page_t *shelf = pages[0].memory;
    assert(depth == 1 && live_pages == 1 && shelf->foreground);
    publish_list(shelf);
    for (unsigned run = 0; run < 10; ++run)
    {
        click(shelf->rows[1]);
        page_t *catalog = pages[1].memory;
        assert(depth == 2 && catalog->foreground && !shelf->foreground);
        assert(!strcmp(catalog->book, "book-B") && !strcmp(catalog->book_title, "Title B"));
        assert(!strcmp(submitted_book, "book-B") && !strcmp(submitted_title, "Title B"));
        publish_list(catalog); ++scenarios;

        deferred = true; click(catalog->rows[1]);
        assert(depth == 2 && queued.callback);
        /* The sender has returned; the borrowed argument must live in its page. */
        assert((uintptr_t)queued.userdata >= (uintptr_t)catalog &&
               (uintptr_t)queued.userdata + sizeof(chapter_selection_t) <= (uintptr_t)(catalog + 1));
        deferred = false; gui_app_exec_now();
        page_t *reader = pages[2].memory;
        assert(depth == 3 && reading == reader && reader->foreground);
        assert(reader->catalog == catalog->result && reader->catalog->references == 2);
        assert(reader->chapter_index == 1 && submitted_index == 1 && submitted_catalog == catalog->result);
        lv_obj_update_layout(reader->screen);
        assert(!strcmp(lv_label_get_text(reader->title), "Chapter 2")); ++scenarios;
        gui_app_goback();
        assert(!reading && catalog->foreground && catalog->result->references == 1);
        publish_list(catalog);
        click(catalog->primary);
        reader = pages[2].memory;
        assert(reader->chapter_index == 0 && submitted_index == 0);
        lv_obj_update_layout(reader->screen);
        assert(!strcmp(lv_label_get_text(reader->title), "Chapter 1")); ++scenarios;
        gui_app_goback(); gui_app_goback();
        assert(depth == 1 && live_pages == 1 && shelf->foreground);
        publish_list(shelf);
    }
    for (int failure = 0; failure < 2; ++failure)
    {
        unsigned before = submissions;
        fail_after = failure; click(shelf->rows[1]);
        page_t *catalog = pages[1].memory;
        assert(!catalog->request && submissions == before);
        assert(strstr(lv_label_get_text(catalog->message), "操作无法开始"));
        gui_app_goback(); publish_list(shelf); ++scenarios;
    }
    click(shelf->rows[1]);
    page_t *catalog = pages[1].memory; publish_list(catalog);
    for (int failure = 0; failure < 2; ++failure)
    {
        unsigned before = submissions;
        fail_after = failure; click(catalog->rows[1]);
        assert(!pages[2].memory->request && submissions == before);
        gui_app_goback();
        assert(!reading && catalog->result->references == 1);
        publish_list(catalog); ++scenarios;
    }
    gui_app_goback();
    dispatch(&pages[0], GUI_APP_MSG_ONPAUSE); dispatch(&pages[0], GUI_APP_MSG_ONSTOP);
    lv_screen_load(empty); lv_obj_delete(pages[0].screen); free(pages[0].memory);
    lv_display_delete(display); lv_deinit();
    assert(!live_pages && !live_allocations && !reading && stops == 1);
    printf("PASS weread UI: %u scenarios, real LVGL + application callbacks, no live allocations\n", scenarios);
}
