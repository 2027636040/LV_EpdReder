#include "books_ui.h"
#include "storage.h"
#include "document/document_view.h"

static lv_obj_t *book_progress_create(lv_obj_t *parent, int x, int y, int width);
static void bookshelf_page_button_update(lv_obj_t *button, bool visible, bool enabled);
static void bookshelf_update(bool reset_focus);
static void bookshelf_create(lv_obj_t *screen);
static void reader_status_render(void);
static void reader_footer_render(void);
static void reader_render(void);
static void reader_update(void);
static void reader_touch_event(lv_event_t *event);
static void reader_create(lv_obj_t *screen);
static void reader_option_render(void);
static void reader_panel_close(void);
static void reader_panel_open(void);
static void reader_toc_open(void);
static void reader_save_position(void);
static void reader_resume(void);

bool reader_session;
static bool reader_touch = true;
static unsigned reader_timeout;
static unsigned saved_bookshelf_first;
static bool reader_page_live;
static uint32_t books_storage_revision;

void bookshelf_start(lv_obj_t *screen)
{
    current->bookshelf_first = saved_bookshelf_first;
    if (!reader_session)
    {
        ui_bookshelf_refresh();
        books_storage_revision = storage_revision();
    }
    bookshelf_create(screen);
}

void bookshelf_resume(void)
{
    if (!reader_session && books_storage_revision != storage_revision())
    {
        ui_bookshelf_refresh();
        books_storage_revision = storage_revision();
    }
    bookshelf_update(false);
}
void bookshelf_pause(void) { saved_bookshelf_first = current->bookshelf_first; }

void reader_start(lv_obj_t *screen)
{
    reader_page_live = true;
    reader_create(screen);
}

void reader_foreground(void)
{
    reader_resume();
}

void reader_pause(void)
{
    if (!reader_session) return;
    ui_reader_cancel_pending();
    current->reader_confirm_pending = false;
    reader_save_position();
}

void reader_stop(void)
{
    reader_page_live = false;
    if (document_view_active()) document_view_detach();
}

bool reader_action(int *request, bool *opened)
{
    (void)opened;
    int target = *request;
    if (launcher_current_page() == UI_PAGE_READER)
    {
        if (target == READER_TOC) { reader_toc_open(); return true; }
        if (target == READER_TOC_CLOSE) { reader_panel_close(); return true; }
        if (current->reader_toc_panel)
        {
            if (target == READER_TOC_PREVIOUS && current->reader_toc_first >= 6) current->reader_toc_first -= 6;
            else if (target == READER_TOC_NEXT && current->reader_toc_first + 6 < document_view_toc_count()) current->reader_toc_first += 6;
            else if (target >= READER_TOC_BASE && target < READER_TOC_BASE + 6)
            {
                unsigned index = current->reader_toc_first + target - READER_TOC_BASE;
                if (document_view_toc_seek(index)) reader_panel_close();
                return true;
            }
            else return true;
            reader_toc_open();
            return true;
        }
        if (target == READER_PREVIOUS || target == READER_NEXT)
        {
            ui_reader_turn(target == READER_PREVIOUS ? -1 : 1);
            ui_reader_process();
            reader_update();
            return true;
        }
        if (target == READER_MENU) { reader_panel_open(); return true; }
        if (current->reader_panel)
        {
            switch (target)
            {
            case READER_OPTION_PREVIOUS: current->reader_option = (current->reader_option + 2) % 3; break;
            case READER_OPTION_NEXT: current->reader_option = (current->reader_option + 1) % 3; break;
            case READER_OPTION_CYCLE:
                if (current->reader_option == 0) reader_touch = !reader_touch;
                else if (current->reader_option == 1)
                {
                    ui_settings_cycle(UI_SETTING_FULL_REFRESH);
                }
                else reader_timeout = (reader_timeout + 1) % 4;
                break;
            case READER_MINUS5: current->reader_jump = current->reader_jump > 5 ? current->reader_jump - 5 : 1; break;
            case READER_MINUS1: if (current->reader_jump > 1) --current->reader_jump; break;
            case READER_PLUS1: ++current->reader_jump; break;
            case READER_PLUS5: current->reader_jump += 5; break;
            case READER_CONFIRM:
                if ((!ui_reader_view()->ready || current->reader_reflow_pending) &&
                    !ui_reader_view()->error[0])
                {
                    pending_page = READER_CONFIRM;
                    return true;
                }
                ui_reader_seek(current->reader_jump);
                current->reader_confirm_pending = true;
                return true;
            default: break;
            }
            if (target <= READER_OPTION_PREVIOUS && target >= READER_PLUS5)
            {
                unsigned last = ui_reader_view()->pages;
                if (last && current->reader_jump > last) current->reader_jump = last;
                if (current->reader_jump > UI_READER_PAGE_MAX) current->reader_jump = UI_READER_PAGE_MAX;
                reader_option_render();
                return true;
            }
        }
    }

    return false;
}

bool bookshelf_action(int *request, bool *opened)
{
    int target = *request;
    if (launcher_current_page() == UI_PAGE_BOOKSHELF)
    {
        if (target == BOOKSHELF_PREVIOUS || target == BOOKSHELF_NEXT)
        {
            if (target == BOOKSHELF_PREVIOUS && current->bookshelf_first > 0)
                current->bookshelf_first -= BOOKSHELF_PAGE_SIZE;
            else if (target == BOOKSHELF_NEXT && current->bookshelf_first + BOOKSHELF_PAGE_SIZE < ui_bookshelf_count())
                current->bookshelf_first += BOOKSHELF_PAGE_SIZE;
            else
                return true;
            bookshelf_update(true);
            return true;
        }
        if (target >= BOOK_OPEN_BASE && target < BOOK_OPEN_BASE + BOOKSHELF_PAGE_SIZE)
        {
            unsigned index = current->bookshelf_first + (unsigned)(target - BOOK_OPEN_BASE);
            if (!ui_bookshelf_book(index)) return true;
            /* ui_reader_open replaces the previous session even when opening fails. */
            reader_session = false;
            if (!ui_reader_open(index))
            {
                const char *detail = document_view_get()->error;
                popup_open("无法打开书籍", detail[0] ? detail : "请检查文件或存储设备");
                return true;
            }
            reader_session = true;
            *opened = true;
            target = UI_PAGE_READER;
        }
    }

    *request = target;
    return false;
}

void books_process(void)
{
    /* Previous iteration already submitted the visible page. Preparation never
     * changes its revision and may overlap the panel's waveform transmission. */
    if (reader_session && launcher_current_page() == UI_PAGE_READER &&
        pending_page == NAV_IDLE && !current->reader_panel && !current->popup &&
        current->reader_revision == ui_reader_view()->revision)
        ui_reader_prepare_process();
    if (reader_session && lv_refreshing_done())
    {
        bool leaving_page = pending_page == NAV_BACK ||
            (pending_page >= UI_PAGE_HOME && pending_page < UI_PAGE_COUNT &&
             pending_page != UI_PAGE_READER);
        if (launcher_current_page() == UI_PAGE_READER && !leaving_page)
            ui_reader_process();
        else
            ui_reader_index_process();
        if (launcher_current_page() == UI_PAGE_READER && current->reader_reflow_pending &&
            document_view_active() && current->reader_revision != ui_reader_view()->revision)
            reader_render();
        if (launcher_current_page() == UI_PAGE_READER && current->reader_reflow_pending &&
            ((ui_reader_view()->ready && !ui_reader_waiting()) || ui_reader_view()->error[0]))
        {
            current->reader_reflow_pending = false;
            /* An untouched menu target follows the restored text position after reflow. */
            if (current->reader_jump == current->reader_reflow_page)
                current->reader_jump = ui_reader_view()->page;
            reader_update();
            if (current->reader_panel) reader_option_render();
        }
        if (!reader_page_live &&
            (document_view_active() || !ui_reader_view()->indexing || ui_reader_view()->error[0]))
        {
            ui_reader_close();
            reader_session = false;
            if (launcher_current_page() == UI_PAGE_BOOKSHELF) bookshelf_resume();
        }
    }
    if (launcher_current_page() == UI_PAGE_READER && (!current->reader_panel || current->reader_confirm_pending) &&
        !current->popup && lv_refreshing_done())
    {
        if (current->reader_confirm_pending)
        {
            if (document_view_active() && current->reader_revision != ui_reader_view()->revision) reader_render();
            const ui_reader_view_t *reading = ui_reader_view();
            unsigned target = reading->pages && current->reader_jump > reading->pages ? reading->pages : current->reader_jump;
            if ((reading->page == target && !ui_reader_waiting()) || reading->error[0])
            {
                reader_panel_close();
                reader_render();
            }
        }
        reader_update();
    }

}

static lv_obj_t *book_progress_create(lv_obj_t *parent, int x, int y, int width)
{
    lv_obj_t *track = panel_create(parent, x, y, width, 8);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_width(track, 0, 0);
    lv_obj_set_style_bg_color(track, lv_color_hex(0xDDDDDD), 0);
    lv_obj_set_style_radius(track, 4, 0);
    lv_obj_t *fill = panel_create(track, 0, 0, 0, 8);
    lv_obj_remove_flag(fill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_width(fill, 0, 0);
    lv_obj_set_style_bg_color(fill, lv_color_hex(0x222222), 0);
    lv_obj_set_style_radius(fill, 4, 0);
    return fill;
}

static void bookshelf_page_button_update(lv_obj_t *button, bool visible, bool enabled)
{
    object_visible(button, visible);
    if (lv_obj_has_state(button, LV_STATE_DISABLED) == !enabled) return;
    if (enabled)
    {
        lv_obj_remove_state(button, LV_STATE_DISABLED);
        lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    }
    else
    {
        lv_obj_add_state(button, LV_STATE_DISABLED);
        lv_obj_remove_flag(button, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_set_style_text_color(lv_obj_get_child(button, 0),
                               lv_color_hex(enabled ? 0x222222 : 0x999999), 0);
}

static void bookshelf_update(bool reset_focus)
{
    static const uint32_t cover_colors[] = {0xEEEEEE, 0xDDDDDD, 0xFFFFFF, 0xCCCCCC};
    unsigned count = ui_bookshelf_count();
    char text[128];
    if (current->bookshelf_first >= count)
        current->bookshelf_first = count ? ((count - 1) / BOOKSHELF_PAGE_SIZE) * BOOKSHELF_PAGE_SIZE : 0;
    object_visible(current->bookshelf_empty, count == 0);
    for (unsigned slot = 0; slot < BOOKSHELF_PAGE_SIZE; ++slot)
    {
        unsigned index = current->bookshelf_first + slot;
        object_visible(current->books[slot].card, index < count);
        if (index >= count) continue;
        const ui_bookshelf_book_t *book = ui_bookshelf_book(index);
        lv_color_t color = lv_color_hex(cover_colors[index % 4]);
        if (!lv_color_eq(lv_obj_get_style_bg_color(current->books[slot].cover, 0), color))
            lv_obj_set_style_bg_color(current->books[slot].cover, color, 0);
        text_update(current->books[slot].title, book->file.name);
        snprintf(text, sizeof(text), "%s · %luKB | 最近阅读：%s", document_format_name(book->file.format),
                 (unsigned long)((book->file.size + 1023u) / 1024u), book->last_read);
        text_update(current->books[slot].metadata, text);
        unsigned percent = book->position.progress > 100 ? 100 : book->position.progress;
        int width = (442 * percent + 50) / 100;
        object_visible(current->books[slot].fill, width > 0);
        if (lv_obj_get_width(current->books[slot].fill) != width)
            lv_obj_set_width(current->books[slot].fill, width);
        if (book->position.current_page && book->position.total_pages)
            snprintf(text, sizeof(text), "进度 %u%% | 第%u/%u页",
                     (unsigned)book->position.progress, (unsigned)book->position.current_page,
                     (unsigned)book->position.total_pages);
        else if (book->position.current_page)
            snprintf(text, sizeof(text), "进度 %u%% | 第%u页",
                     (unsigned)book->position.progress, (unsigned)book->position.current_page);
        else snprintf(text, sizeof(text), "未读");
        text_update(current->books[slot].progress, text);
    }
    bookshelf_page_button_update(current->bookshelf_previous, count > 0, current->bookshelf_first > 0);
    bookshelf_page_button_update(current->bookshelf_next, count > 0,
                                 current->bookshelf_first + BOOKSHELF_PAGE_SIZE < count);
    if (reset_focus || !focus_eligible(lv_group_get_focused(current->group))) focus_restore(0);
}

static void bookshelf_create(lv_obj_t *screen)
{
    page_title_create(screen, "书架", ui_font_title());
    for (unsigned slot = 0; slot < BOOKSHELF_PAGE_SIZE; ++slot)
    {
        lv_obj_t *card = button_create(screen, MARGIN, 178 + slot * 210,
                                       CONTENT_WIDTH, 190, NULL, BOOK_OPEN_BASE + (int)slot);
        current->books[slot].card = card;
        lv_obj_t *cover = panel_create(card, 20, 22, 112, 146);
        current->books[slot].cover = cover;
        lv_obj_remove_flag(cover, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(cover, 4, 0);
        icon_create(cover, &ui_icon_bookshelf, 15, 32);
        current->books[slot].title = label_create(card, "", 152, 22, 442, ui_font_body());
        current->books[slot].metadata = label_create(card, "", 152, 68, 442, ui_font_small());
        current->books[slot].fill = book_progress_create(card, 152, 115, 442);
        current->books[slot].progress = label_create(card, "", 152, 141, 442, ui_font_caption());
    }
    current->bookshelf_empty = centered_label(screen, "这里空空如也~", MARGIN,
        (PAGE_CONTENT_Y + HEIGHT - MARGIN - ui_font_body()->line_height) / 2,
        CONTENT_WIDTH, ui_font_body());
    current->bookshelf_previous = button_create(screen, MARGIN, HEIGHT - MARGIN - 70,
                                                298, 70, "上一页", BOOKSHELF_PREVIOUS);
    current->bookshelf_next = button_create(screen, 354, HEIGHT - MARGIN - 70,
                                            298, 70, "下一页", BOOKSHELF_NEXT);
    lv_obj_t *buttons[] = {current->bookshelf_previous, current->bookshelf_next};
    for (unsigned i = 0; i < 2; ++i)
    {
        lv_obj_set_style_border_color(buttons[i], lv_color_hex(0xBBBBBB), LV_STATE_DISABLED);
    }
    bookshelf_update(false);
}

static void reader_status_render(void)
{
    /* Update with reader operations, never from the periodic status poll. */
    char text[32];
    epd_app_status_t status;
    epd_app_status(&status);
    snprintf(text, sizeof(text), "电量：%d%%", status.battery_percent);
    text_update(current->reader_battery, text);
    text_update(current->reader_clock, status.clock);
}

static void reader_footer_render(void)
{
    const ui_reader_view_t *reading = ui_reader_view();
    if (!current->reader_footer) return;
    char text[196];
    if (reading->error[0]) text_update(current->reader_footer, reading->error);
    else if (!reading->ready) text_update(current->reader_footer, "");
    else
    {
        if (!reading->saved)
            snprintf(text, sizeof(text), "进度未保存 · %u 页", reading->page);
        else if (reading->pages)
            snprintf(text, sizeof(text), "%u / %u 页 · %u%%", reading->page, reading->pages,
                     reading->percent);
        else
            snprintf(text, sizeof(text), "%u / -- 页 · %u%%", reading->page, reading->percent);
        text_update(current->reader_footer, text);
    }
    current->reader_rendered_pages = reading->pages;
}

static void reader_render(void)
{
    const ui_reader_view_t *reading = ui_reader_view();
    if (!current->reader_body) return;
    if (current->reader_title && reading->title) text_update(current->reader_title, reading->title);
    reader_status_render();
    if (document_view_active())
    {
        if (document_view_bind(current->reader_body)) text_update(current->reader_body, "");
        else if (!reading->ready && !reading->error[0]) text_update(current->reader_body, "正在准备文档，请稍候…");
    }
    else if (!reading->error[0] && !reading->ready)
        text_update(current->reader_body, "正在排版，请稍候…");
    else if (!reading->error[0])
    {
        text_update(current->reader_body, reading->text[0] ? reading->text : "本书暂无正文");
    }
    if (reading->ready && !reading->error[0])
    {
        int width = CONTENT_WIDTH * reading->percent / 100;
        if (width) lv_obj_remove_flag(current->reader_progress, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(current->reader_progress, LV_OBJ_FLAG_HIDDEN);
        if (lv_obj_get_width(current->reader_progress) != width) lv_obj_set_width(current->reader_progress, width);
    }
    reader_footer_render();
    current->reader_revision = reading->revision;
}

static void reader_update(void)
{
    const ui_reader_view_t *reading = ui_reader_view();
    if (reading->revision != current->reader_revision) reader_render();
    else if (reading->ready && !reading->indexing && reading->pages != current->reader_rendered_pages)
    {
        /* A completed index updates only the footer; no body, clock or progress invalidation. */
        reader_footer_render();
    }
    if (!reading->error[0]) current->reader_error_reported = false;
    else if (!current->reader_error_reported && !current->popup)
    {
        reader_panel_close();
        popup_open(reading->error, "请返回书架重新打开");
        current->reader_error_reported = true;
    }
}

static void reader_touch_event(lv_event_t *event)
{
    if (storage_changing()) return;
    if (!current || !current->foreground || current->id != UI_PAGE_READER ||
        lv_obj_get_screen(lv_event_get_target_obj(event)) != current->screen) return;
    if (pending_page != NAV_IDLE || current->reader_panel || current->popup || !lv_refreshing_done()) return;
    if (ui_reader_waiting()) return;
    if (!reader_touch || !ui_settings_enabled(UI_SETTING_TOUCH)) return;
    lv_indev_t *indev = lv_event_get_indev(event);
    if (!indev) return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    pending_page = point.x < WIDTH / 3 ? READER_PREVIOUS :
                   point.x > WIDTH * 2 / 3 ? READER_NEXT : READER_MENU;
}

static void reader_create(lv_obj_t *screen)
{
    const ui_reader_view_t *reading = ui_reader_view();
    const lv_font_t *title_font = ui_font_small();
    current->reader_title = label_create(screen, reading->title, PAGE_TITLE_X, (UI_READER_TOP - title_font->line_height) / 2,
                                         WIDTH - MARGIN - PAGE_TITLE_X, title_font);
    current->reader_body = label_create(screen, "", reading->margin, UI_READER_TOP,
                                WIDTH - 2 * reading->margin, reading->font);
    lv_label_set_long_mode(current->reader_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(current->reader_body, UI_READER_BOTTOM - UI_READER_TOP);
    lv_obj_set_style_text_line_space(current->reader_body, reading->line_space, 0);
    lv_obj_set_style_text_letter_space(current->reader_body, 0, 0);
    lv_obj_set_style_text_color(current->reader_body, lv_color_black(), 0);
    lv_obj_add_flag(current->reader_body, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(current->reader_body, reader_touch_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *track = panel_create(screen, MARGIN, UI_READER_BOTTOM + 6, CONTENT_WIDTH, 4);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(track, 0, 0);
    current->reader_progress = panel_create(track, 0, 0, 0, 4);
    lv_obj_remove_flag(current->reader_progress, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(current->reader_progress, 0, 0);
    lv_obj_set_style_border_width(current->reader_progress, 0, 0);
    lv_obj_set_style_bg_color(current->reader_progress, lv_color_black(), 0);
    const lv_font_t *status_font = ui_font_body();
    const lv_font_t *page_font = ui_font_caption();
    int footer_y = HEIGHT - 8 - status_font->line_height;
    current->reader_battery = label_create(screen, "", 8, footer_y, 180, status_font);
    current->reader_footer = centered_label(screen, "", 200,
                                   footer_y + (status_font->line_height - page_font->line_height) / 2,
                                   WIDTH - 400, page_font);
    current->reader_clock = label_create(screen, "", WIDTH - 188, footer_y, 180, status_font);
    lv_obj_set_style_text_align(current->reader_clock, LV_TEXT_ALIGN_RIGHT, 0);
    reader_render();
}

static void reader_option_render(void)
{
    if (current->reader_toc_panel || !current->reader_option_label || !current->reader_jump_label) return;
    char text[96];
    static const char *const timeouts[] = {"5分钟", "10分钟", "30分钟", "不关机"};
    if (current->reader_option == 0) snprintf(text, sizeof(text), "触摸翻页：%s", reader_touch ? "开" : "关");
    else if (current->reader_option == 1)
        snprintf(text, sizeof(text), "全刷周期：%s", ui_settings_value(UI_SETTING_FULL_REFRESH));
    else snprintf(text, sizeof(text), "超时关机：%s", timeouts[reader_timeout]);
    text_update(current->reader_option_label, text);
    snprintf(text, sizeof(text), "%u", current->reader_jump);
    text_update(current->reader_jump_label, text);
}

static void reader_panel_close(void)
{
    if (!current->reader_panel) return;
    lv_group_remove_all_objs(current->group);
    lv_obj_delete(current->reader_panel);
    current->reader_panel = current->reader_option_label = current->reader_jump_label = NULL;
    current->reader_confirm_pending = false;
    current->reader_toc_panel = false;
    while (current->focus_count > current->reader_panel_focus_start) current->focus_items[--current->focus_count] = NULL;
    for (unsigned i = 0; i < current->focus_count; ++i) lv_group_add_obj(current->group, current->focus_items[i]);
    if (current->focus_count) lv_group_focus_obj(current->focus_items[0]);
    reader_status_render();
}

static void reader_panel_open(void)
{
    if (current->reader_panel || !ui_reader_view()->ready) return;
    reader_status_render();
    current->reader_panel_focus_start = current->focus_count;
    current->reader_jump = ui_reader_view()->page;
    current->reader_panel = panel_create(lv_screen_active(), MARGIN, 798, CONTENT_WIDTH, 390);
    lv_obj_set_style_border_color(current->reader_panel, lv_color_black(), 0);
    lv_obj_set_style_border_width(current->reader_panel, 2, 0);
    centered_label(current->reader_panel, "阅读设置", 18, 20, 584, ui_font_body());
    button_create(current->reader_panel, 18, 74, 60, 66, "<", READER_OPTION_PREVIOUS);
    lv_obj_t *choice = button_create(current->reader_panel, 88, 74, 444, 66, NULL, READER_OPTION_CYCLE);
    current->reader_option_label = centered_label(choice, "", 8, 18, 424, ui_font_body());
    button_create(current->reader_panel, 542, 74, 60, 66, ">", READER_OPTION_NEXT);
    button_create(current->reader_panel, 18, 162, 94, 66, "-5", READER_MINUS5);
    button_create(current->reader_panel, 122, 162, 94, 66, "-1", READER_MINUS1);
    current->reader_jump_label = centered_label(current->reader_panel, "", 226, 180, 168, ui_font_body());
    button_create(current->reader_panel, 404, 162, 94, 66, "+1", READER_PLUS1);
    button_create(current->reader_panel, 508, 162, 94, 66, "+5", READER_PLUS5);
    button_create(current->reader_panel, 18, 268, 282, 74, "确认", READER_CONFIRM);
    if (document_view_toc_count())
    {
        button_create(current->reader_panel, 318, 268, 134, 74, "目录", READER_TOC);
        button_create(current->reader_panel, 462, 268, 138, 74, "文本设置", UI_PAGE_TEXT_SETTINGS);
    }
    else button_create(current->reader_panel, 318, 268, 282, 74, "文本设置", UI_PAGE_TEXT_SETTINGS);
    reader_option_render();
    lv_group_focus_obj(current->focus_items[current->reader_panel_focus_start]);
}

static void reader_toc_open(void)
{
    reader_panel_close();
    current->reader_panel_focus_start = current->focus_count;
    current->reader_toc_panel = true;
    current->reader_panel = panel_create(lv_screen_active(), MARGIN, 170, CONTENT_WIDTH, 1018);
    lv_obj_set_style_border_color(current->reader_panel, lv_color_black(), 0);
    lv_obj_set_style_border_width(current->reader_panel, 2, 0);
    centered_label(current->reader_panel, "目录", 18, 22, 584, ui_font_body());
    unsigned count = document_view_toc_count();
    for (unsigned i = 0; i < 6 && current->reader_toc_first + i < count; ++i)
    {
        char title[256], indented[276];
        unsigned level;
        if (!document_view_toc_title(current->reader_toc_first + i, title, sizeof(title), &level)) continue;
        unsigned indent = level > 1 ? (level > 5 ? 4 : level - 1) : 0;
        snprintf(indented, sizeof(indented), "%*s%s", (int)(indent * 2), "", title);
        lv_obj_t *button = button_create(current->reader_panel, 18, 88 + i * 128, 584, 108,
                                         indented, READER_TOC_BASE + (int)i);
        lv_obj_t *label = lv_obj_get_child(button, 0);
        if (label) { lv_obj_set_width(label, 552); lv_label_set_long_mode(label, LV_LABEL_LONG_DOT); }
    }
    button_create(current->reader_panel, 18, 902, 184, 76, "上一组", READER_TOC_PREVIOUS);
    button_create(current->reader_panel, 218, 902, 184, 76, "关闭", READER_TOC_CLOSE);
    button_create(current->reader_panel, 418, 902, 184, 76, "下一组", READER_TOC_NEXT);
    lv_group_focus_obj(current->focus_items[current->reader_panel_focus_start]);
}

static void reader_save_position(void)
{
    const ui_reader_view_t *reading = ui_reader_view();
    if (!reading->ready) return;
    ui_reader_save();
}

static void reader_resume(void)
{
    if (ui_reader_settings_changed())
    {
        current->reader_reflow_page = ui_reader_view()->page;
        current->reader_reflow_pending = true;
        current->reader_error_reported = false;
        /* Reflow may release the old font. Detach it before touching the font cache. */
        lv_obj_set_style_text_font(current->reader_body, ui_font_body(), 0);
        ui_reader_reflow();
        const ui_reader_view_t *reading = ui_reader_view();
        lv_obj_set_style_text_font(current->reader_body,
                                  reading->font ? reading->font : ui_font_body(), 0);
        if (!document_view_active())
        {
            lv_obj_set_x(current->reader_body, reading->margin);
            lv_obj_set_width(current->reader_body, WIDTH - 2 * reading->margin);
            lv_obj_set_style_text_line_space(current->reader_body, reading->line_space, 0);
        }
        ui_reader_process();
        reader_render();
    }
    else
    {
        ui_reader_process();
        reader_update();
    }
    reader_status_render();
    if (current->reader_panel) reader_option_render();
}
