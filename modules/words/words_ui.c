#include "words_service.h"
#include <string.h>
#include <stdio.h>

typedef struct
{
    lv_obj_t *screen, *input, *keyboard, *status, *search, *rows[WORDS_MATCH_MAX];
    lv_timer_t *timer;
    words_result_t *displayed;
    words_job_t selected;
    uint32_t request;
} lookup_t;

typedef struct
{
    lv_obj_t *screen, *word, *phonetic, *body, *status, *previous, *next, *collect, *pause;
    lv_timer_t *timer;
    words_job_t job;
    words_result_t *displayed;
    char *text;
    uint32_t request;
    size_t offset, end, *history;
    unsigned page, history_capacity;
} detail_t;

static void detail_message(gui_app_msg_type_t message, void *parameter);

static void visible(lv_obj_t *object, bool show)
{
    if (show) lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *words_prepare_back(const char *title, lv_event_cb_t back, void *data)
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
    if (back) epd_app_back_button_cb(screen, back, data);
    else epd_app_back_button(screen);
    epd_app_label(screen, title, 104, 103, 540, epd_app_font_role(EPD_FONT_TITLE));
    return screen;
}

lv_obj_t *words_prepare(const char *title) { return words_prepare_back(title, NULL, NULL); }

static void select_word(lv_event_t *event)
{
    lookup_t *page = lv_event_get_user_data(event);
    if (!page->displayed || page->request) return;
    for (unsigned i = 0; i < page->displayed->matches.count; ++i)
    {
        if (page->rows[i] != lv_event_get_target_obj(event)) continue;
        memset(&page->selected, 0, sizeof(page->selected));
        page->selected.kind = WORDS_DETAIL;
        page->selected.entry = page->displayed->matches.entry[i];
        strcpy(page->selected.path, page->displayed->path);
        memcpy(page->selected.identity, page->displayed->identity, sizeof(page->selected.identity));
        gui_app_create_page_for_app_ext("words", "detail", detail_message, &page->selected, sizeof(detail_t));
        break;
    }
}

static void lookup_poll(lv_timer_t *timer)
{
    lookup_t *page = lv_timer_get_user_data(timer);
    bool failed;
    words_result_t *result = words_take(page->request, &failed);
    if (!result && !failed) return;
    lv_timer_pause(timer);
    page->request = 0;
    lv_obj_remove_state(page->search, LV_STATE_DISABLED);
    epd_app_free(page->displayed);
    page->displayed = result;
    if (!result || result->error[0])
    {
        epd_app_text(page->status, result ? result->error : "内存不足");
        return;
    }
    for (unsigned i = 0; i < result->matches.count; ++i)
    {
        epd_app_text(lv_obj_get_child(page->rows[i], 0), result->names[i]);
        visible(page->rows[i], true);
    }
    epd_app_text(page->status, !result->matches.count ? "没有找到这个单词" :
                 (result->matches.more ? "请补充更多字母" : ""));
}

static void search(lv_event_t *event)
{
    lookup_t *page = lv_event_get_user_data(event);
    if (page->request) return;
    const char *text = lv_textarea_get_text(page->input);
    if (!text[0]) return;
    words_job_t job = {.kind = WORDS_SEARCH};
    snprintf(job.query, sizeof(job.query), "%s", text);
    size_t length = strlen(job.query);
    while (length && job.query[length - 1] == ' ') job.query[--length] = 0;
    size_t first = 0;
    while (job.query[first] == ' ') ++first;
    if (first) memmove(job.query, job.query + first, length - first + 1);
    if (!job.query[0]) return;
    visible(page->keyboard, false);
    for (unsigned i = 0; i < WORDS_MATCH_MAX; ++i) visible(page->rows[i], false);
    lv_obj_add_state(page->search, LV_STATE_DISABLED);
    epd_app_text(page->status, "正在查询");
    page->request = words_submit(&job);
    if (page->request) lv_timer_resume(page->timer);
    else { epd_app_text(page->status, "任务繁忙，请重试"); lv_obj_remove_state(page->search, LV_STATE_DISABLED); }
}

static void edit(lv_event_t *event)
{
    lookup_t *page = lv_event_get_user_data(event);
    if (!page->request) visible(page->keyboard, true);
}

static void keyboard_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_READY) search(event);
    else if (lv_event_get_code(event) == LV_EVENT_CANCEL)
    {
        lookup_t *page = lv_event_get_user_data(event);
        visible(page->keyboard, false);
    }
}

void words_lookup_message(gui_app_msg_type_t message, void *parameter)
{
    (void)parameter;
    lookup_t *page = gui_app_this_page_memory();
    if (message == GUI_APP_MSG_ONSTART)
    {
        page->screen = words_prepare("查词");
        page->input = lv_textarea_create(page->screen);
        epd_app_object_init(page->input);
        lv_obj_set_style_bg_color(page->input, lv_color_white(), 0);
        lv_obj_set_style_border_width(page->input, 1, 0);
        lv_obj_set_style_border_color(page->input, lv_color_black(), 0);
        lv_obj_set_style_pad_left(page->input, 12, 0);
        lv_obj_set_style_pad_top(page->input, 20, 0);
        lv_obj_set_pos(page->input, 32, 188);
        lv_obj_set_size(page->input, 470, 88);
        lv_textarea_set_one_line(page->input, true);
        lv_textarea_set_max_length(page->input, WORDS_KEY_SIZE - 1);
        lv_textarea_set_accepted_chars(page->input,
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 '-.&()/+");
        lv_obj_set_style_anim_duration(page->input, 0, 0);
        lv_obj_set_style_anim_duration(page->input, 0, LV_PART_CURSOR);
        lv_obj_set_style_bg_opa(page->input, LV_OPA_TRANSP, LV_PART_CURSOR);
        lv_obj_set_style_border_width(page->input, 0, LV_PART_CURSOR);
        lv_obj_set_style_shadow_width(page->input, 0, LV_PART_CURSOR);
        lv_obj_add_event_cb(page->input, edit, LV_EVENT_CLICKED, page);
        page->search = epd_app_button(page->screen, 518, 188, 134, 88, "查询", search, page);
        page->status = epd_app_label(page->screen, "", 32, 292, 620, epd_app_font_role(EPD_FONT_SMALL));
        for (unsigned i = 0; i < WORDS_MATCH_MAX; ++i)
        {
            page->rows[i] = epd_app_button(page->screen, 32, 354 + i * 99, 620, 84, "", select_word, page);
            visible(page->rows[i], false);
        }
        page->keyboard = lv_keyboard_create(page->screen);
        epd_app_object_init(page->keyboard);
        lv_obj_set_size(page->keyboard, 684, 450);
        lv_obj_set_pos(page->keyboard, 0, 766);
        lv_keyboard_set_textarea(page->keyboard, page->input);
        lv_keyboard_set_mode(page->keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
        lv_obj_set_style_text_font(page->keyboard, epd_app_font(), LV_PART_ITEMS);
        lv_obj_set_style_text_color(page->keyboard, lv_color_black(), LV_PART_ITEMS);
        lv_obj_set_style_bg_color(page->keyboard, lv_color_white(), LV_PART_ITEMS);
        lv_obj_set_style_bg_opa(page->keyboard, LV_OPA_COVER, LV_PART_ITEMS);
        lv_obj_set_style_border_color(page->keyboard, lv_color_black(), LV_PART_ITEMS);
        lv_obj_set_style_border_width(page->keyboard, 1, LV_PART_ITEMS);
        lv_obj_set_style_pad_top(page->keyboard, 8, 0);
        lv_obj_set_style_pad_bottom(page->keyboard, 8, 0);
        lv_obj_set_style_pad_left(page->keyboard, 8, 0);
        lv_obj_set_style_pad_right(page->keyboard, 8, 0);
        lv_obj_set_style_pad_row(page->keyboard, 8, 0);
        lv_obj_set_style_bg_color(page->keyboard, lv_color_white(), LV_PART_ITEMS | LV_STATE_PRESSED);
        lv_obj_set_style_border_color(page->keyboard, lv_color_black(), LV_PART_ITEMS | LV_STATE_PRESSED);
        lv_obj_set_style_border_width(page->keyboard, 3, LV_PART_ITEMS | LV_STATE_PRESSED);
        lv_obj_set_style_anim_duration(page->keyboard, 0, 0);
        lv_obj_add_event_cb(page->keyboard, keyboard_event, LV_EVENT_ALL, page);
        page->timer = lv_timer_create(lookup_poll, 100, page);
        lv_timer_pause(page->timer);
    }
    else if (message == GUI_APP_MSG_ONRESUME)
    {
        epd_app_header_refresh(page->screen);
    }
    else if (message == GUI_APP_MSG_ONPAUSE)
    {
        lv_timer_pause(page->timer);
        words_cancel(page->request);
        page->request = 0;
        lv_obj_remove_state(page->search, LV_STATE_DISABLED);
    }
    else if (message == GUI_APP_MSG_ONSTOP)
    {
        lv_timer_delete(page->timer);
        epd_app_free(page->displayed);
    }
}

static void detail_render(detail_t *page)
{
    size_t total = strlen(page->text), low = 1, high = total - page->offset;
    const lv_font_t *font = epd_app_font();
    size_t best = 0;
    while (low <= high)
    {
        size_t length = low + (high - low) / 2;
        size_t boundary = page->offset + length;
        while (boundary < total && ((unsigned char)page->text[boundary] & 0xc0) == 0x80) --boundary;
        char saved = page->text[boundary];
        page->text[boundary] = 0;
        lv_point_t size;
        lv_text_get_size(&size, page->text + page->offset, font, 0, 8, 620, LV_TEXT_FLAG_NONE);
        page->text[boundary] = saved;
        if (size.y <= 568) { best = boundary - page->offset; low = length + 1; }
        else high = length - 1;
    }
    if (!best && page->offset < total)
    {
        best = 1;
        while (page->offset + best < total &&
               ((unsigned char)page->text[page->offset + best] & 0xc0) == 0x80) ++best;
    }
    page->end = page->offset + best;
    char saved = page->text[page->end];
    page->text[page->end] = 0;
    epd_app_text(page->body, page->text + page->offset);
    page->text[page->end] = saved;
    visible(page->previous, page->page > 0);
    visible(page->next, page->end < total);
}

static void detail_turn(lv_event_t *event)
{
    detail_t *page = lv_event_get_user_data(event);
    if (!page->text) return;
    if (lv_event_get_target_obj(event) == page->previous)
    {
        if (!page->page) return;
        page->offset = page->history[--page->page];
    }
    else
    {
        if (page->end >= strlen(page->text)) return;
        if (page->page == page->history_capacity)
        {
            unsigned capacity = page->history_capacity ? page->history_capacity * 2 : 16;
            size_t *history = epd_app_alloc(capacity * sizeof(*history), EPD_APP_PSRAM);
            if (!history) { epd_app_text(page->status, "内存不足"); return; }
            if (page->history) memcpy(history, page->history, page->page * sizeof(*history));
            epd_app_free(page->history);
            page->history = history;
            page->history_capacity = capacity;
        }
        page->history[page->page++] = page->offset;
        page->offset = page->end;
    }
    detail_render(page);
}

static void detail_poll(lv_timer_t *timer)
{
    detail_t *page = lv_timer_get_user_data(timer);
    bool failed;
    words_result_t *result = words_take(page->request, &failed);
    if (!result && !failed) return;
    lv_timer_pause(timer);
    page->request = 0;
    if (!result || result->error[0])
    {
        epd_app_text(page->status, result ? result->error : "内存不足");
        epd_app_free(result);
        return;
    }
    epd_app_free(page->displayed);
    page->displayed = result;
    page->job.slot = result->slot;
    epd_app_text(lv_obj_get_child(page->collect, 0), (result->flags & WORDS_COLLECTED) ? "移出生词本" : "加入生词本");
    epd_app_text(lv_obj_get_child(page->pause, 0), (result->flags & WORDS_PAUSED) ? "恢复学习" : "暂停学习");
    visible(page->collect, true); visible(page->pause, true);
    epd_app_text(page->status, "");
    if (page->text) return;
    epd_app_text(page->word, result->fields[WORDS_WORD]);
    epd_app_text(page->phonetic, result->fields[WORDS_PHONETIC]);
    size_t length = strlen(result->fields[WORDS_TRANSLATION]) + strlen(result->fields[WORDS_DEFINITION]) +
                    strlen(result->fields[WORDS_EXCHANGE]) + 8;
    page->text = epd_app_alloc(length, EPD_APP_PSRAM);
    if (!page->text) { epd_app_text(page->status, "内存不足"); return; }
    snprintf(page->text, length, "%s\n\n%s\n\n%s", result->fields[WORDS_TRANSLATION],
             result->fields[WORDS_DEFINITION], result->fields[WORDS_EXCHANGE]);
    epd_app_text(page->status, "");
    detail_render(page);
}

static void detail_action(lv_event_t *event)
{
    detail_t *page = lv_event_get_user_data(event);
    if (!page->displayed || page->request) return;
    words_job_t job = page->job;
    bool collect = lv_event_get_target_obj(event) == page->collect;
    job.kind = collect ? WORDS_COLLECT : WORDS_PAUSE;
    job.value = !(page->displayed->flags & (collect ? WORDS_COLLECTED : WORDS_PAUSED));
    page->request = words_submit(&job);
    if (page->request) lv_timer_resume(page->timer);
    else epd_app_text(page->status, "任务繁忙，请重试");
}

static void detail_message(gui_app_msg_type_t message, void *parameter)
{
    (void)parameter;
    detail_t *page = gui_app_this_page_memory();
    if (message == GUI_APP_MSG_ONSTART)
    {
        page->job = *(const words_job_t *)gui_app_this_page_userdata();
        page->screen = words_prepare("单词");
        page->word = epd_app_label(page->screen, "", 32, 204, 620, epd_app_font_role(EPD_FONT_TITLE));
        lv_obj_set_height(page->word, 64);
        lv_label_set_long_mode(page->word, LV_LABEL_LONG_DOT);
        page->phonetic = epd_app_label(page->screen, "", 32, 282, 620, epd_app_font());
        lv_obj_set_height(page->phonetic, 60);
        lv_label_set_long_mode(page->phonetic, LV_LABEL_LONG_DOT);
        page->collect = epd_app_button(page->screen, 32, 354, 294, 76, "加入生词本", detail_action, page);
        page->pause = epd_app_button(page->screen, 358, 354, 294, 76, "暂停学习", detail_action, page);
        visible(page->collect, false); visible(page->pause, false);
        page->status = epd_app_label(page->screen, "正在读取", 32, 444, 620, epd_app_font_role(EPD_FONT_SMALL));
        page->body = epd_app_label(page->screen, "", 32, 496, 620, epd_app_font());
        lv_obj_set_height(page->body, 568);
        lv_label_set_long_mode(page->body, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_letter_space(page->body, 0, 0);
        lv_obj_set_style_text_line_space(page->body, 8, 0);
        page->previous = epd_app_button(page->screen, 32, 1100, 294, 76, "上一页", detail_turn, page);
        page->next = epd_app_button(page->screen, 358, 1100, 294, 76, "下一页", detail_turn, page);
        visible(page->previous, false);
        visible(page->next, false);
        page->timer = lv_timer_create(detail_poll, 100, page);
        lv_timer_pause(page->timer);
    }
    else if (message == GUI_APP_MSG_ONRESUME)
    {
        epd_app_header_refresh(page->screen);
        page->job.kind = WORDS_DETAIL;
        page->request = words_submit(&page->job);
        if (page->request) lv_timer_resume(page->timer);
        else epd_app_text(page->status, "任务繁忙，请重试");
    }
    else if (message == GUI_APP_MSG_ONPAUSE)
    {
        lv_timer_pause(page->timer);
        words_cancel(page->request);
        page->request = 0;
    }
    else if (message == GUI_APP_MSG_ONSTOP)
    {
        lv_timer_delete(page->timer);
        epd_app_free(page->text);
        epd_app_free(page->history);
        epd_app_free(page->displayed);
    }
}

void words_open_detail(const words_job_t *job)
{
    gui_app_create_page_for_app_ext("words", "detail", detail_message, (void *)job, sizeof(detail_t));
}

void words_open_lookup(void)
{
    gui_app_create_page_for_app_ext("words", "lookup", words_lookup_message, NULL, sizeof(lookup_t));
}
