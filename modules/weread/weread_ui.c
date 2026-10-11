#include "weread_service.h"
#include "weread_resources.h"
#include "weread_covers.h"
#include <rthw.h>
#include <string.h>
#include <stdio.h>

EPD_APP_DEFINE("weread");
#define LIST_ROWS 8

typedef enum { PAGE_SHELF, PAGE_CATALOG, PAGE_READER } page_kind_t;
typedef struct { weread_result_t *catalog; size_t index; } chapter_selection_t;
typedef struct
{
    weread_result_t *content;
    bool cancelled;
} reader_context_t;
typedef struct page
{
    page_kind_t kind;
    bool foreground, reader_opened, awaiting_service, chapter_pending, reader_notice, failure_notified;
    lv_obj_t *screen, *message, *title, *rows[LIST_ROWS], *labels[LIST_ROWS];
    lv_obj_t *covers[LIST_ROWS];
    weread_cover_t *cover_data[LIST_ROWS];
    lv_obj_t *previous, *next, *primary, *secondary, *qr, *body, *footer, *battery, *clock, *menu;
    lv_obj_t *verify_panel, *verify_digits[4], *verify_keys[12];
    char otp[5];
    lv_timer_t *timer;
    weread_result_t *result, *catalog;
    reader_context_t *reader_context;
    uint32_t request, revision, reader_revision, cover_request;
    size_t first, selected, chapter_index, requested_chapter;
    chapter_selection_t selection;
    char *book, *book_title;
} page_t;

static page_t *reading;
static unsigned live_pages;
static void catalog_message(gui_app_msg_type_t message, void *parameter);
static void reader_message(gui_app_msg_type_t message, void *parameter);
static void poll(lv_timer_t *timer);

static void show(lv_obj_t *object, bool visible)
{
    if (!object || lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN) == !visible) return;
    if (visible) lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}

static void verify_digits(page_t *page)
{
    size_t length = strlen(page->otp);
    for (unsigned i = 0; i < 4; ++i)
    {
        char text[2] = {i < length ? page->otp[i] : '-', 0};
        epd_app_text(page->verify_digits[i], text);
    }
}

static void verify_hide(page_t *page)
{
    if (!page->verify_panel) return;
    bool visible = !lv_obj_has_flag(page->verify_panel, LV_OBJ_FLAG_HIDDEN);
    memset(page->otp, 0, sizeof(page->otp));
    verify_digits(page);
    show(page->verify_panel, false);
    if (visible) lv_group_focus_obj(page->primary);
}

static lv_obj_t *prepare(const char *title)
{
    gui_app_close_anim();
    lv_obj_t *screen = lv_screen_active();
    epd_app_object_init(screen);
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);
    lv_obj_set_style_text_font(screen, epd_app_font(), 0);
    epd_app_input_group(screen);
    epd_app_header(screen);
    epd_app_back_button(screen);
    lv_obj_t *label = epd_app_label(screen, title, 104, 103, 548, epd_app_font_role(EPD_FONT_TITLE));
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    return screen;
}

static void loading(page_t *page, uint32_t request)
{
    if (page->kind == PAGE_SHELF) weread_covers_cancel();
    page->request = request;
    page->failure_notified = false;
    if (page->kind == PAGE_READER)
    {
        page->chapter_pending = request != 0;
        page->reader_notice = true;
    }
    epd_app_text(page->message, request ? "正在读取" : "操作无法开始，请稍后重试");
    show(page->message, true);
}

static void cover_clear(page_t *page, unsigned row)
{
    if (page->covers[row]) lv_obj_delete(page->covers[row]);
    page->covers[row] = NULL;
    weread_covers_free(page->cover_data[row]);
    page->cover_data[row] = NULL;
}

static bool cover_matches(const weread_cover_t *cover, const weread_result_t *snapshot, size_t index)
{
    return cover && index < snapshot->book_count && !strcmp(cover->account, snapshot->account) &&
           !strcmp(cover->book, snapshot->books[index].id) && !strcmp(cover->url, snapshot->books[index].cover);
}

static void list_render(page_t *page)
{
    weread_result_t *result = page->result;
    size_t count = page->kind == PAGE_SHELF ? result->book_count : result->chapter_count;
    if (page->first >= count) page->first = 0;
    for (unsigned i = 0; i < LIST_ROWS; ++i)
    {
        size_t index = page->first + i;
        if (page->kind == PAGE_SHELF && !cover_matches(page->cover_data[i], result, index)) cover_clear(page, i);
        show(page->rows[i], index < count);
        if (index < count)
            epd_app_text(page->labels[i], page->kind == PAGE_SHELF ? result->books[index].title : result->chapters[index].title);
    }
    show(page->previous, page->first != 0);
    show(page->next, count - page->first > LIST_ROWS);
    show(page->message, !count);
    if (!count) epd_app_text(page->message, page->kind == PAGE_SHELF ? "书架暂无图书" : "暂无章节");
    if (page->kind == PAGE_SHELF)
        page->cover_request = weread_covers_request(result, page->first, LIST_ROWS);
}

static void open_chapter(page_t *page, size_t index)
{
    if (!page->result || index >= page->result->chapter_count) return;
    /* app_fwk borrows user_data. The paused catalog page owns this selection. */
    page->selection = (chapter_selection_t){page->result, index};
    gui_app_create_page_for_app_ext("weread", "reader", reader_message, &page->selection, sizeof(page_t));
    gui_app_exec_now();
}

static void list_clicked(lv_event_t *event)
{
    page_t *page = lv_event_get_user_data(event);
    if (!page->result || !page->foreground || page->result->generation != page->request) return;
    unsigned row;
    for (row = 0; row < LIST_ROWS; ++row) if (page->rows[row] == lv_event_get_target_obj(event)) break;
    size_t index = page->first + row;
    if (row == LIST_ROWS) return;
    page->selected = row;
    if (page->kind == PAGE_SHELF)
    {
        if (index >= page->result->book_count) return;
        gui_app_create_page_for_app_ext("weread", "catalog", catalog_message, &page->result->books[index], sizeof(page_t));
        gui_app_exec_now();
    }
    else open_chapter(page, index);
}

static void list_turn(lv_event_t *event)
{
    page_t *page = lv_event_get_user_data(event);
    if (!page->result || page->result->generation != page->request) return;
    if (lv_event_get_target_obj(event) == page->previous)
        page->first = page->first >= LIST_ROWS ? page->first - LIST_ROWS : 0;
    else page->first += LIST_ROWS;
    list_render(page);
    lv_group_focus_obj(page->rows[0]);
}

static void shelf_action(lv_event_t *event)
{
    page_t *page = lv_event_get_user_data(event);
    if (!weread_start())
    {
        page->awaiting_service = weread_stopping();
        loading(page, 0);
        return;
    }
    bool authenticated = page->result && page->result->account && page->result->account[0] &&
                         page->result->action != WEREAD_LOGIN && page->result->result != WR_ERR_AUTH &&
                         page->result->result != WR_EXPIRED;
    weread_action_t action = authenticated ? WEREAD_SHELF : WEREAD_LOGIN;
    if (lv_event_get_target_obj(event) == page->secondary) action = WEREAD_LOGOUT;
    uint32_t request = weread_submit(action, NULL, NULL, NULL, NULL);
    if (request) verify_hide(page);
    loading(page, request);
}

static void verify_key(lv_event_t *event)
{
    page_t *page = lv_event_get_user_data(event);
    if (!page->foreground || !page->result || !page->result->verification ||
        page->result->generation != page->request) return;
    unsigned key;
    for (key = 0; key < 12; ++key)
        if (page->verify_keys[key] == lv_event_get_target_obj(event)) break;
    size_t length = strlen(page->otp);
    if (key == 11)
    {
        if (length != 4)
        { epd_app_text(page->message, "请输入四位验证码"); return; }
        uint32_t request = weread_submit_code(page->request, page->otp);
        if (!request)
        {
            epd_app_text(page->message, "验证码暂时无法提交，请重试");
            return;
        }
        loading(page, request);
        epd_app_text(page->message, "正在验证");
        memset(page->otp, 0, sizeof(page->otp));
    }
    else if (key == 9)
    {
        if (length) page->otp[length - 1] = 0;
    }
    else if (key < 11 && length < 4)
    {
        page->otp[length] = key == 10 ? '0' : (char)('1' + key);
        page->otp[length + 1] = 0;
    }
    verify_digits(page);
}

static void verify_show(page_t *page)
{
    if (!page->verify_panel)
    {
        page->verify_panel = lv_obj_create(page->screen);
        epd_app_object_init(page->verify_panel);
        lv_obj_remove_style_all(page->verify_panel);
        lv_obj_remove_flag(page->verify_panel, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(page->verify_panel, 32, 200);
        lv_obj_set_size(page->verify_panel, 620, 846);
        epd_app_centered_label(page->verify_panel, "双重验证码", 0, 10, 620,
                               epd_app_font_role(EPD_FONT_TITLE));
        for (unsigned i = 0; i < 4; ++i)
        {
            lv_obj_t *box = lv_obj_create(page->verify_panel);
            epd_app_object_init(box);
            lv_obj_remove_style_all(box);
            lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_pos(box, 66 + i * 128, 96);
            lv_obj_set_size(box, 104, 88);
            lv_obj_set_style_border_color(box, lv_color_black(), 0);
            lv_obj_set_style_border_width(box, 1, 0);
            page->verify_digits[i] = epd_app_centered_label(box, "-", 0, 18, 102,
                                                          epd_app_font_role(EPD_FONT_TITLE));
        }
        static const char *const keys[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "删除", "0", "确认"};
        for (unsigned i = 0; i < 12; ++i)
            page->verify_keys[i] = epd_app_button(page->verify_panel, 10 + (i % 3) * 204,
                320 + (i / 3) * 110, 192, 96, keys[i], verify_key, page);
    }
    memset(page->otp, 0, sizeof(page->otp));
    verify_digits(page);
    show(page->verify_panel, true);
    lv_obj_set_y(page->message, 432);
    epd_app_text(lv_obj_get_child(page->primary, 0), "重新扫码");
    lv_group_focus_obj(page->verify_keys[0]);
}

static void catalog_continue(lv_event_t *event)
{
    page_t *page = lv_event_get_user_data(event);
    if (!page->result || page->result->generation != page->request) return;
    size_t index = 0;
    if (page->result->last_chapter)
        for (size_t i = 0; i < page->result->chapter_count; ++i)
            if (!strcmp(page->result->last_chapter, page->result->chapters[i].uid)) { index = i; break; }
    open_chapter(page, index);
}

static void list_create(page_t *page, const char *title)
{
    ++live_pages;
    page->screen = prepare(title);
    page->message = epd_app_centered_label(page->screen, "", 32, 534, 620, epd_app_font());
    for (unsigned i = 0; i < LIST_ROWS; ++i)
    {
        page->rows[i] = epd_app_button(page->screen, 32, 224 + i * 96, 620, 84, NULL, list_clicked, page);
        page->labels[i] = epd_app_label(page->rows[i], "", page->kind == PAGE_SHELF ? 84 : 20,
            20, page->kind == PAGE_SHELF ? 512 : 572, epd_app_font());
        lv_label_set_long_mode(page->labels[i], LV_LABEL_LONG_DOT);
        show(page->rows[i], false);
    }
    page->previous = epd_app_button(page->screen, 32, 1014, 294, 72, "上一页", list_turn, page);
    page->next = epd_app_button(page->screen, 358, 1014, 294, 72, "下一页", list_turn, page);
    show(page->previous, false); show(page->next, false);
    page->timer = lv_timer_create(poll, 100, page);
    lv_timer_pause(page->timer);
}

static void page_release(page_t *page)
{
    memset(page->otp, 0, sizeof(page->otp));
    if (page->timer) lv_timer_delete(page->timer);
    if (page->kind == PAGE_SHELF)
    {
        weread_covers_cancel();
        for (unsigned i = 0; i < LIST_ROWS; ++i) cover_clear(page, i);
    }
    weread_cancel(page->request);
    weread_result_free(page->result); weread_result_free(page->catalog);
    epd_app_free(page->book); epd_app_free(page->book_title);
    if (--live_pages == 0) weread_stop();
}

static void shelf_message(gui_app_msg_type_t message, void *parameter)
{
    (void)parameter;
    page_t *page = gui_app_this_page_memory();
    if (message == GUI_APP_MSG_ONSTART)
    {
        page->kind = PAGE_SHELF;
        list_create(page, "微信读书");
        page->qr = lv_qrcode_create(page->screen);
        lv_qrcode_set_size(page->qr, 360);
        lv_obj_set_pos(page->qr, 162, 288);
        show(page->qr, false);
        page->primary = epd_app_button(page->screen, 32, 1100, 294, 76, "登录", shelf_action, page);
        page->secondary = epd_app_button(page->screen, 358, 1100, 294, 76, "退出账号", shelf_action, page);
        show(page->secondary, false);
        bool started = weread_start();
        page->awaiting_service = !started && weread_stopping();
        loading(page, started ? weread_submit(WEREAD_RESTORE, NULL, NULL, NULL, NULL) : 0);
    }
    else if (message == GUI_APP_MSG_ONRESUME)
    {
        page->foreground = true;
        epd_app_header_refresh(page->screen);
        lv_timer_resume(page->timer);
        if (page->result && page->result->action != WEREAD_LOGIN && page->result->result == WR_OK)
            loading(page, weread_submit(WEREAD_RESTORE, NULL, NULL, NULL, NULL));
        if (page->result && page->result->book_count) lv_group_focus_obj(page->rows[page->selected]);
    }
    else if (message == GUI_APP_MSG_ONPAUSE)
    { page->foreground = false; lv_timer_pause(page->timer); weread_covers_cancel(); }
    else if (message == GUI_APP_MSG_ONSTOP) page_release(page);
}

static void catalog_message(gui_app_msg_type_t message, void *parameter)
{
    (void)parameter;
    page_t *page = gui_app_this_page_memory();
    if (message == GUI_APP_MSG_ONSTART)
    {
        const wr_book_t *book = gui_app_this_page_userdata();
        page->kind = PAGE_CATALOG;
        page->book = weread_string(book->id); page->book_title = weread_string(book->title);
        list_create(page, book->title);
        page->primary = epd_app_button(page->screen, 32, 1100, 620, 76, "继续阅读", catalog_continue, page);
        show(page->primary, false);
        loading(page, page->book && page->book_title ? weread_submit(WEREAD_CATALOG, page->book, page->book_title, NULL, NULL) : 0);
    }
    else if (message == GUI_APP_MSG_ONRESUME)
    {
        page->foreground = true; lv_timer_resume(page->timer); epd_app_header_refresh(page->screen);
        if (page->result)
            loading(page, weread_submit(WEREAD_CATALOG_RESTORE, page->book, page->book_title, NULL, NULL));
        if (page->result && page->result->chapter_count) lv_group_focus_obj(page->rows[page->selected]);
    }
    else if (message == GUI_APP_MSG_ONPAUSE)
    { page->foreground = false; lv_timer_pause(page->timer); }
    else if (message == GUI_APP_MSG_ONSTOP) page_release(page);
}

static bool content_cancelled(void *user)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool cancel = ((reader_context_t *)user)->cancelled;
    rt_hw_interrupt_enable(level);
    return cancel || weread_unavailable();
}
static bool content_save(void *user, const reader_position_t *position)
{ return weread_save_position(((reader_context_t *)user)->content, position); }
static void content_release(void *user)
{
    reader_context_t *context = user;
    weread_result_free(context->content); epd_app_free(context);
}
static int content_resource(void *user, const char *base, const char *reference, book_stream_t *stream)
{
    return weread_resource_open_ex(((reader_context_t *)user)->content,
        base, reference, stream, content_cancelled, user);
}
static void reader_close(page_t *page)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (page->reader_context) page->reader_context->cancelled = true;
    page->reader_context = NULL;
    rt_hw_interrupt_enable(level);
    epd_reader_close();
    page->reader_opened = false;
}

static void reader_begin(page_t *page, weread_result_t *result)
{
    char cache[STORAGE_PATH_MAX];
    if (!weread_store_path(cache, STORAGE_APP_CACHE, result->account, result->book, "layout"))
    {
        epd_app_text(page->message, "阅读缓存不可用，请检查存储后重试");
        return;
    }
    reader_options_t options = {0};
    reader_context_t *context = epd_app_alloc(sizeof(*context), EPD_APP_PSRAM);
    if (!context)
    {
        epd_app_text(page->message, "阅读器内存不足，请返回重试");
        return;
    }
    *context = (reader_context_t){.content = result};
    options.app_id = "weread"; options.cache_path = cache;
    options.content_identity = result->path; options.path = result->path;
    options.title = result->chapter_title; options.kind = result->html ? READER_SOURCE_XHTML : READER_SOURCE_TXT;
    options.restore = result->restore;
    options.resources = (book_resources_t){context, content_resource};
    options.cancelled = content_cancelled; options.save = content_save; options.user = context;
    options.release_user = content_release;
    weread_result_retain(result);
    reader_close(page);
    page->reader_opened = epd_reader_open(&options);
    if (!page->reader_opened) epd_app_text(page->message, "阅读器启动失败，请返回重试");
    else
    {
        page->reader_context = context; page->chapter_index = page->requested_chapter;
        epd_app_text(page->title, result->chapter_title);
        reading = page; page->reader_revision = 0; weread_wake();
    }
}

static void change_chapter(page_t *page, int direction)
{
    if (!page->catalog || (direction < 0 && !page->chapter_index) ||
        (direction > 0 && page->chapter_index + 1 >= page->catalog->chapter_count)) return;
    epd_reader_save();
    page->requested_chapter = direction < 0 ? page->chapter_index - 1 : page->chapter_index + 1;
    loading(page, weread_submit_chapter(page->catalog, page->requested_chapter));
}

static void reader_turn(page_t *page, int direction)
{
    if (!page->reader_opened || page->menu || page->chapter_pending) return;
    const reader_view_t *view = epd_reader_get();
    if (!view->ready || epd_reader_waiting()) return;
    page->reader_notice = false;
    if (direction < 0 && view->page <= 1) change_chapter(page, -1);
    else if (direction > 0 && view->pages && view->page >= view->pages) change_chapter(page, 1);
    else if (direction < 0) epd_reader_seek(view->page - 1);
    else if (view->page < READER_PAGE_MAX) epd_reader_seek(view->page + 1);
    weread_wake();
}

static void body_tap(lv_event_t *event)
{
    page_t *page = lv_event_get_user_data(event);
    lv_indev_t *input = lv_event_get_indev(event);
    if (!input) return;
    lv_point_t point;
    lv_indev_get_point(input, &point);
    reader_turn(page, point.x < 228 ? -1 : 1);
}

static void turn_button(lv_event_t *event)
{
    page_t *page = lv_event_get_user_data(event);
    reader_turn(page, lv_event_get_target_obj(event) == page->previous ? -1 : 1);
}

static void menu_close(page_t *page)
{
    if (page->menu) lv_obj_delete(page->menu);
    page->menu = NULL;
}

static void menu_action(lv_event_t *event)
{
    page_t *page = reading;
    if (!page) return;
    int action = (int)(intptr_t)lv_event_get_user_data(event);
    menu_close(page);
    if (action == 1) { epd_reader_save(); epd_app_open_text_settings(); }
    else if (action == 2) change_chapter(page, -1);
    else if (action == 3) change_chapter(page, 1);
    else if (action == 4) gui_app_goback();
}

static void reader_back(lv_event_t *event)
{
    (void)event;
    gui_app_goback();
}

static void menu_open(lv_event_t *event)
{
    page_t *page = lv_event_get_user_data(event);
    if (page->menu) { menu_close(page); return; }
    page->menu = epd_app_panel(page->screen, 70, 290, 544, 610);
    epd_app_input_group(page->menu);
    const char *labels[] = {"继续阅读", "文本设置", "上一章", "下一章", "章节目录"};
    for (unsigned i = 0; i < 5; ++i)
    {
        lv_obj_t *button = epd_app_button(page->menu, 32, 30 + i * 112, 480, 88, labels[i], menu_action, (void *)(intptr_t)i);
        if (!i) { epd_app_input_back(page->menu, button); lv_group_focus_obj(button); }
    }
}

static void reader_message(gui_app_msg_type_t message, void *parameter)
{
    (void)parameter;
    page_t *page = gui_app_this_page_memory();
    if (message == GUI_APP_MSG_ONSTART)
    {
        const chapter_selection_t *selection = gui_app_this_page_userdata();
        page->kind = PAGE_READER; ++live_pages;
        page->catalog = selection->catalog; weread_result_retain(page->catalog);
        page->chapter_index = selection->index;
        page->requested_chapter = selection->index;
        page->book = weread_string(page->catalog->book); page->book_title = weread_string(page->catalog->title);
        const wr_chapter_t *chapter = &page->catalog->chapters[page->chapter_index];
        page->screen = lv_screen_active(); epd_app_object_init(page->screen);
        lv_obj_set_style_bg_color(page->screen, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(page->screen, LV_OPA_COVER, 0);
        epd_app_input_group(page->screen);
        lv_obj_t *back = epd_app_button(page->screen, 18, 2, 78, 54, "返回", reader_back, page);
        epd_app_input_back(page->screen, back);
        page->title = epd_app_label(page->screen, chapter->title, 100, 9, 460, epd_app_font_role(EPD_FONT_SMALL));
        lv_label_set_long_mode(page->title, LV_LABEL_LONG_DOT);
        epd_app_button(page->screen, 578, 2, 88, 54, "菜单", menu_open, page);
        page->body = epd_app_panel(page->screen, 0, READER_TOP, 684, READER_BOTTOM - READER_TOP);
        lv_obj_set_style_border_width(page->body, 0, 0); lv_obj_set_style_pad_all(page->body, 0, 0);
        lv_obj_add_flag(page->body, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(page->body, body_tap, LV_EVENT_CLICKED, page);
        page->message = epd_app_centered_label(page->screen, "", 32, 534, 620, epd_app_font());
        page->footer = epd_app_centered_label(page->screen, "", 160, 1170, 364, epd_app_font_role(EPD_FONT_SMALL));
        page->battery = epd_app_label(page->screen, "", 20, 1170, 150, epd_app_font_role(EPD_FONT_SMALL));
        page->clock = epd_app_label(page->screen, "", 554, 1170, 110, epd_app_font_role(EPD_FONT_SMALL));
        /* Invisible focus targets keep the development board's global keys usable. */
        page->previous = epd_app_button(page->screen, 0, 1152, 150, 64, NULL, turn_button, page);
        page->next = epd_app_button(page->screen, 534, 1152, 150, 64, NULL, turn_button, page);
        lv_obj_set_style_bg_opa(page->previous, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(page->previous, 0, 0);
        lv_obj_set_style_bg_opa(page->next, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(page->next, 0, 0);
        page->timer = lv_timer_create(poll, 100, page); lv_timer_pause(page->timer);
        reading = page;
        loading(page, page->book && page->book_title ? weread_submit_chapter(page->catalog, page->chapter_index) : 0);
    }
    else if (message == GUI_APP_MSG_ONRESUME)
    {
        reading = page; page->foreground = true; lv_timer_resume(page->timer);
        if (page->reader_opened && epd_reader_settings_changed()) epd_reader_reflow();
    }
    else if (message == GUI_APP_MSG_ONPAUSE)
    { page->foreground = false; epd_reader_save(); lv_timer_pause(page->timer); }
    else if (message == GUI_APP_MSG_ONSTOP)
    {
        epd_reader_save(); reader_close(page);
        if (reading == page) reading = NULL;
        page_release(page);
    }
}

static void poll(lv_timer_t *timer)
{
    page_t *page = lv_timer_get_user_data(timer);
    if (!page->foreground) return;
    if (page->kind == PAGE_SHELF && epd_app_refresh_done())
    {
        weread_cover_t *cover = weread_covers_take();
        if (cover && page->result && cover->request == page->cover_request &&
            cover->generation == page->request && cover->revision == page->result->revision &&
            cover->index >= page->first && cover->index - page->first < LIST_ROWS &&
            cover_matches(cover, page->result, cover->index))
        {
            unsigned row = (unsigned)(cover->index - page->first);
            if (!cover_matches(page->cover_data[row], page->result, cover->index))
            {
                cover_clear(page, row);
                page->covers[row] = lv_image_create(page->rows[row]);
                if (page->covers[row])
                {
                    page->cover_data[row] = cover;
                    lv_image_set_src(page->covers[row], &cover->image);
                    lv_obj_set_pos(page->covers[row], 12 + (WEREAD_COVER_WIDTH - cover->image.header.w) / 2,
                                   6 + (WEREAD_COVER_HEIGHT - cover->image.header.h) / 2);
                    cover = NULL;
                }
            }
        }
        weread_covers_free(cover);
    }
    if (page->awaiting_service && !weread_stopping())
    {
        page->awaiting_service = false;
        loading(page, weread_start() ? weread_submit(WEREAD_RESTORE, NULL, NULL, NULL, NULL) : 0);
    }
    if (!page->failure_notified && weread_failed(page->request))
    {
        page->failure_notified = true;
        page->chapter_pending = false;
        if (page->kind == PAGE_SHELF)
        {
            verify_hide(page);
            show(page->qr, false);
            epd_app_text(lv_obj_get_child(page->primary, 0), "登录");
        }
        epd_app_text(page->message, "内存不足，请返回重试");
        show(page->message, true);
    }
    weread_result_t *result = weread_result_get();
    if (result && result->generation == page->request && result->revision != page->revision)
    {
        page->chapter_pending = false;
        page->revision = result->revision;
        weread_result_free(page->result); page->result = result; result = NULL;
        weread_result_t *snapshot = page->result;
        if (page->kind == PAGE_SHELF && snapshot->action == WEREAD_LOGIN && snapshot->verification)
        {
            for (unsigned i = 0; i < LIST_ROWS; ++i) show(page->rows[i], false);
            show(page->previous, false); show(page->next, false); show(page->secondary, false);
            show(page->qr, false);
            verify_show(page);
            epd_app_text(page->message, snapshot->error[0] ? snapshot->error : "请输入手机上显示的四位验证码");
            show(page->message, true);
        }
        else if (snapshot->result < 0 || snapshot->result == WR_EXPIRED ||
                 snapshot->result == WR_VERIFY_EXPIRED || snapshot->result == WR_VERIFY_LIMIT)
        {
            epd_app_text(page->message, snapshot->error[0] ? snapshot->error : "操作失败，请返回重试");
            show(page->message, true);
            if (page->kind != PAGE_READER)
            {
                for (unsigned i = 0; i < LIST_ROWS; ++i) show(page->rows[i], false);
                show(page->previous, false); show(page->next, false);
                if (page->kind == PAGE_CATALOG) show(page->primary, false);
            }
            if (page->kind == PAGE_SHELF)
            {
                verify_hide(page);
                show(page->qr, false);
                if (snapshot->result == WR_ERR_AUTH || snapshot->result == WR_EXPIRED || snapshot->action == WEREAD_LOGIN)
                    epd_app_text(lv_obj_get_child(page->primary, 0), "登录");
            }
        }
        else if (page->kind == PAGE_READER)
        {
            reader_begin(page, snapshot);
        }
        else if (page->kind == PAGE_SHELF && snapshot->action == WEREAD_LOGIN)
        {
            verify_hide(page);
            epd_app_text(lv_obj_get_child(page->primary, 0), "重新扫码");
            for (unsigned i = 0; i < LIST_ROWS; ++i) show(page->rows[i], false);
            show(page->previous, false); show(page->next, false); show(page->secondary, false);
            if (snapshot->qr)
            {
                bool ready = lv_qrcode_update(page->qr, snapshot->qr, strlen(snapshot->qr)) == LV_RESULT_OK;
                show(page->qr, ready);
                lv_obj_set_y(page->message, 724);
                epd_app_text(page->message, ready ? "使用微信扫码登录" : "二维码生成失败，请重新登录");
                show(page->message, true);
                if (!ready) weread_cancel(page->request);
            }
        }
        else
        {
            verify_hide(page);
            show(page->qr, false); lv_obj_set_y(page->message, 534);
            list_render(page);
            if (page->kind == PAGE_SHELF)
            {
                lv_obj_t *label = lv_obj_get_child(page->primary, 0);
                epd_app_text(label, "刷新书架"); show(page->secondary, true);
            }
            else show(page->primary, snapshot->chapter_count != 0);
        }
    }
    weread_result_free(result);
}

bool weread_ui_process(epd_service_t *service, bool stopping)
{
    if (stopping)
    {
        if (!epd_app_refresh_done()) return false;
        if (reading) reader_close(reading);
        else epd_reader_close();
        epd_reader_collect();
        return true;
    }
    epd_reader_collect();
    if (!reading || !reading->foreground || !reading->reader_opened || reading->menu ||
        reading->chapter_pending || !epd_app_refresh_done()) return false;
    reader_work_t work = epd_reader_step(4, true, true);
    weread_wake();
    if (work == READER_WORK_TARGET) epd_service_ui_request(service, 0);
    const reader_view_t *view = epd_reader_get();
    if (view->error[0]) { epd_app_text(reading->message, view->error); show(reading->message, true); }
    else
    {
        if (reading->reader_revision != view->revision && epd_reader_bind(reading->body))
        {
            reading->reader_revision = view->revision;
            epd_app_status_t status; epd_app_status(&status);
            char status_text[32];
            snprintf(status_text, sizeof(status_text), "电量：%d%%", status.battery_percent);
            epd_app_text(reading->battery, status_text); epd_app_text(reading->clock, status.clock);
        }
        if (!view->ready) return false;
        if (reading->reader_context && reading->reader_context->content == reading->result &&
            reading->result->generation == reading->request &&
            reading->result->result == WR_OK) reading->reader_notice = false;
        show(reading->message, reading->reader_notice); show(reading->body, true);
        char text[64];
        if (view->pages) snprintf(text, sizeof(text), "%u / %u", view->page, view->pages);
        else snprintf(text, sizeof(text), "%u", view->page);
        epd_app_text(reading->footer, text);
    }
    return false;
}

int app_main(intent_t intent)
{
    (void)intent;
    return gui_app_regist_msg_handler_ext("weread", shelf_message, NULL, sizeof(page_t));
}
