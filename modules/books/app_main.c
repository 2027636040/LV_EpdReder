#include "books_ui.h"
#include "reader.h"
#include "document/document.h"

EPD_APP_DEFINE("books");
books_page_t *current;
int pending_page = NAV_IDLE;
static bool initialized, initialization_attempted;
static unsigned live_pages, saved_focus[UI_PAGE_COUNT];
static uint32_t storage_generation;
static const char *const page_names[] = {"root", "reader"};
static void page_message(gui_app_msg_type_t message, void *parameter);

static bool page_input_ready(uint32_t key)
{
    if (!initialized || !current || !current->foreground || pending_page != NAV_IDLE)
        return false;
    if (launcher_current_page() == UI_PAGE_READER && !current->popup)
    {
        if (current->reader_confirm_pending && key != LV_KEY_ENTER && key != LV_KEY_ESC)
            return false;
        if (!current->reader_panel && ui_reader_waiting() && key != LV_KEY_ENTER)
            return false;
    }
    return true;
}

static bool page_input_move(const epd_input_event_t *events, unsigned count)
{
    if (!current || !current->foreground || !count) return true;
    if (launcher_current_page() == UI_PAGE_READER && !current->reader_panel && !current->popup)
    {
        const ui_reader_view_t *reading = ui_reader_view();
        int64_t target = reading->page;
        unsigned last = reading->pages ? reading->pages : UI_READER_PAGE_MAX;
        for (unsigned i = 0; i < count; ++i)
        {
            target += events[i].key == LV_KEY_PREV ? -(int64_t)events[i].repeat : events[i].repeat;
            if (target < 1) target = 1;
            if (target > last) target = last;
        }
        /* One bounded target; indexing can catch up without displaying intermediate pages. */
        if (target != (int)reading->page) ui_reader_seek((unsigned)target);
        return true;
    }

    return false;
}

static bool page_input_key(uint32_t key)
{
    if (!initialized || !current || !current->foreground ||
        pending_page != NAV_IDLE) return true;
    if (launcher_current_page() == UI_PAGE_READER)
    {
        if (current->reader_confirm_pending)
        {
            if (key == LV_KEY_ESC || key == LV_KEY_ENTER) pending_page = NAV_BACK;
            return true;
        }
        if (!current->reader_panel && !current->popup)
        {
            if (ui_reader_waiting() && key != LV_KEY_ENTER) return true;
            switch (key)
            {
            case LV_KEY_ENTER: pending_page = NAV_BACK; break;
            case LV_KEY_ESC: pending_page = READER_MENU; break;
            default: break;
            }
            return true;
        }
        if (current->reader_panel && key == LV_KEY_ESC)
        {
            pending_page = current->reader_toc_panel ? READER_TOC_CLOSE : READER_CONFIRM;
            return true;
        }
    }
    if (key == LV_KEY_ESC)
    {
        if (current->popup) pending_page = POPUP_CLOSE;
        else pending_page = NAV_BACK;
        return true;
    }
    return false;
}


static const epd_input_ops_t input_ops = {page_input_ready, page_input_move, page_input_key};

static unsigned focus_index(void)
{
    lv_obj_t *focused = lv_group_get_focused(current->group);
    for (unsigned i = 0; i < current->focus_count; ++i)
        if (current->focus_items[i] == focused) return i;
    return 0;
}

static void clicked(lv_event_t *event)
{
    if (storage_changing() || !current || !current->foreground ||
        lv_obj_get_screen(lv_event_get_target_obj(event)) != current->screen ||
        pending_page != NAV_IDLE) return;
    int target = (int)(intptr_t)lv_event_get_user_data(event);
    if (current->id == UI_PAGE_READER &&
        (!lv_refreshing_done() || (current->reader_confirm_pending && target != NAV_BACK))) return;
    lv_group_focus_obj(lv_event_get_target_obj(event));
    pending_page = target;
}

lv_obj_t *button_create(lv_obj_t *parent, int x, int y, int width, int height, const char *text, int target)
{
    lv_obj_t *button = epd_app_button(parent, x, y, width, height, text,
                                     clicked, (void *)(intptr_t)target);
    LV_ASSERT(current->focus_count < FOCUS_CAPACITY);
    current->focus_items[current->focus_count++] = button;
    return button;
}

lv_obj_t *page_title_create(lv_obj_t *screen, const char *text, const lv_font_t *font)
{
    return label_create(screen, text, PAGE_TITLE_X, PAGE_TITLE_Y,
                         WIDTH - MARGIN - PAGE_TITLE_X, font);
}

void object_visible(lv_obj_t *object, bool visible)
{
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN) == !visible) return;
    if (visible) lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}

void focus_restore(unsigned index)
{
    if (index < current->focus_count && focus_eligible(current->focus_items[index]))
    {
        lv_group_focus_obj(current->focus_items[index]);
        return;
    }
    for (unsigned i = 0; i < current->focus_count; ++i)
        if (focus_eligible(current->focus_items[i]))
        {
            lv_group_focus_obj(current->focus_items[i]);
            return;
        }
}

void popup_close(void)
{
    if (!current->popup) return;
    lv_obj_delete(current->popup);
    current->popup = NULL;
    current->popup_text = current->popup_button = NULL;
    while (current->focus_count > current->popup_focus_start)
        current->focus_items[--current->focus_count] = NULL;
    for (unsigned i = 0; i < current->focus_count; ++i) lv_group_add_obj(current->group, current->focus_items[i]);
    if (focus_eligible(current->popup_previous_focus)) lv_group_focus_obj(current->popup_previous_focus);
    else focus_restore(0);
    current->popup_previous_focus = NULL;
}

void popup_open(const char *text, const char *detail)
{
    if (current->popup) return;
    current->popup_focus_start = current->focus_count;
    current->popup_previous_focus = lv_group_get_focused(current->group);
    lv_group_remove_all_objs(current->group);
    current->popup = panel_create(lv_screen_active(), 0, 0, WIDTH, HEIGHT);
    lv_obj_add_flag(current->popup, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(current->popup, 0, 0);
    lv_obj_set_style_border_width(current->popup, 0, 0);
    lv_obj_set_style_bg_color(current->popup, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(current->popup, LV_OPA_TRANSP, 0);
    lv_obj_t *dialog = panel_create(current->popup, 54, 438, 576, 282);
    lv_obj_set_style_border_color(dialog, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_width(dialog, 2, 0);
    current->popup_text = centered_label(dialog, text, 24, detail ? 44 : 74, 528, ui_font_body());
    if (detail) centered_label(dialog, detail, 24, 102, 528, ui_font_small());
    current->popup_button = button_create(dialog, 108, 184, 360, 64, "确定", POPUP_CLOSE);
    lv_group_focus_obj(current->popup_button);
}

static bool books_initialize(void)
{
    if (initialized) return true;
    initialization_attempted = true;
    if (bookshelf_service_init() != RT_EOK) return false;
    if (reader_service_init() != RT_EOK)
    {
        bookshelf_service_deinit();
        return false;
    }
    ui_bookshelf_refresh();
    const ui_bookshelf_book_t *recent = NULL;
    for (unsigned i = 0; i < ui_bookshelf_count(); ++i)
    {
        const ui_bookshelf_book_t *book = ui_bookshelf_book(i);
        if (book->position.current_page && (!recent || (int32_t)(book->order - recent->order) > 0))
            recent = book;
    }
    if (recent) epd_app_set_recent_reading(recent->file.name, recent->position.progress);
    storage_generation = storage_revision();
    initialized = true;
    return true;
}

static bool page_open(unsigned page)
{
    if (page == UI_PAGE_TEXT_SETTINGS) return epd_app_open_text_settings();
    if (page >= sizeof(page_names) / sizeof(page_names[0])) return false;
    int result = gui_app_is_page_present((char *)page_names[page]) ?
        gui_app_goback_to_page(page_names[page]) :
        gui_app_create_page_for_app_ext("books", page_names[page], page_message,
                                        (void *)(uintptr_t)page, sizeof(books_page_t));
    if (result != RT_EOK) return false;
    gui_app_exec_now();
    return current && current->foreground && current->id == page;
}

static void process_navigation(void)
{
    if (!current || !current->foreground || pending_page == NAV_IDLE || !lv_refreshing_done()) return;
    int target = pending_page;
    pending_page = NAV_IDLE;
    bool opened = false;
    bool handled = current->id == UI_PAGE_BOOKSHELF ?
        bookshelf_action(&target, &opened) : reader_action(&target, &opened);
    if (handled) return;
    if (target == POPUP_CLOSE) { popup_close(); return; }
    bool navigated;
    if (target == NAV_BACK)
    {
        books_page_t *previous = current;
        navigated = gui_app_goback() == RT_EOK;
        if (navigated)
        {
            gui_app_exec_now();
            navigated = current != previous;
        }
    }
    else navigated = page_open((unsigned)target);
    if (!navigated)
    {
        if (opened)
        {
            ui_reader_close();
            reader_session = false;
        }
        if (current && current->foreground) popup_open("页面打开失败，请重试", NULL);
    }
}

static bool service_process(epd_service_t *service, bool stopping)
{
    (void)service;
    document_service_process();
    if (stopping)
    {
        if (live_pages || !lv_refreshing_done()) return false;
        ui_reader_close();
        document_service_shutdown();
        ui_bookshelf_close();
        reader_service_deinit();
        bookshelf_service_deinit();
        initialized = false;
        return true;
    }
    if (storage_changing()) return false;
    if (!initialization_attempted) books_initialize();
    if (!initialized) return false;
    if (storage_generation != storage_revision())
    {
        storage_generation = storage_revision();
        if (launcher_current_page() == UI_PAGE_BOOKSHELF) bookshelf_resume();
    }
    books_process();
    process_navigation();
    return false;
}

static bool books_flush(void)
{
    return ui_reader_save();
}

const epd_background_t epd_app_background = {
    .run = document_worker,
    .stack_size = 32768,
    .priority = 22,
    .command = books_command,
    .ui_process = service_process,
    .flush = books_flush
};

static void page_message(gui_app_msg_type_t message, void *parameter)
{
    (void)parameter;
    books_page_t *page = gui_app_this_page_memory();
    books_page_t *previous = current;
    current = page;
    switch (message)
    {
    case GUI_APP_MSG_ONSTART:
    {
        ++live_pages;
        gui_app_close_anim();
        page->id = (unsigned)(uintptr_t)gui_app_this_page_userdata();
        page->screen = lv_screen_active();
        page->group = epd_app_input_group(page->screen);
        epd_input_set_ops(page->group, &input_ops);
        epd_obj_init(page->screen);
        lv_obj_set_size(page->screen, WIDTH, HEIGHT);
        lv_obj_set_style_bg_color(page->screen, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(page->screen, LV_OPA_COVER, 0);
        lv_obj_set_style_text_font(page->screen, ui_font_body(), 0);
        if (page->id == UI_PAGE_READER) reader_start(page->screen);
        else
        {
            epd_app_header(page->screen);
            bookshelf_start(page->screen);
        }
        lv_obj_t *back = epd_app_back_button_cb(page->screen, clicked, (void *)(intptr_t)NAV_BACK);
        if (page->id == UI_PAGE_READER) lv_obj_set_y(back, (UI_READER_TOP - ICON_BUTTON_SIZE) / 2);
        page->focus_items[page->focus_count++] = back;
        focus_restore(saved_focus[page->id]);
        current = previous;
        break;
    }
    case GUI_APP_MSG_ONRESUME:
        page->foreground = true;
        if (page->id == UI_PAGE_READER) reader_foreground();
        else
        {
            epd_app_header_refresh(page->screen);
            bookshelf_resume();
        }
        break;
    case GUI_APP_MSG_ONPAUSE:
        saved_focus[page->id] = focus_index();
        if (page->id == UI_PAGE_READER) reader_pause();
        else bookshelf_pause();
        page->foreground = false;
        pending_page = NAV_IDLE;
        current = NULL;
        break;
    case GUI_APP_MSG_ONSTOP:
        if (page->id == UI_PAGE_READER) reader_stop();
        --live_pages;
        current = previous == page ? NULL : previous;
        break;
    default:
        current = previous;
        break;
    }
}

int app_main(intent_t intent)
{
    (void)intent;
    if (!books_initialize()) return -RT_ENOMEM;
    return gui_app_regist_msg_handler_ext("books", page_message,
                                          (void *)(uintptr_t)UI_PAGE_BOOKSHELF, sizeof(books_page_t));
}
