#include "words_service.h"
#include <stdio.h>
#include <string.h>

EPD_APP_DEFINE("words");
lv_obj_t *words_prepare_back(const char *title, lv_event_cb_t back, void *data);
void words_open_detail(const words_job_t *job);
void words_open_lookup(void);

typedef enum { PAGE_HOME, PAGE_STUDY, PAGE_COLLECTION, PAGE_SCOPES, PAGE_SETTINGS } page_kind_t;
typedef struct
{
    page_kind_t kind;
    lv_obj_t *screen, *status, *heading, *phonetic, *body, *info;
    lv_obj_t *buttons[WORDS_MATCH_MAX], *previous, *next;
    lv_timer_t *timer;
    words_result_t *result;
    words_job_t child;
    uint32_t request, offset, quota;
    unsigned start_attempts;
    bool root, started, answer;
} page_t;

static void message(gui_app_msg_type_t message, void *parameter);
static void click(lv_event_t *event);
static void show(lv_obj_t *object, bool visible)
{
    if (visible) lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}
static void button_text(lv_obj_t *button, const char *text) { epd_app_text(lv_obj_get_child(button, 0), text); }

static void request(page_t *page, const words_job_t *job)
{
    page->request = words_submit(job);
    if (page->request) lv_timer_resume(page->timer);
    else epd_app_text(page->status, "任务繁忙，请重试");
}
static void refresh(page_t *page)
{
    words_job_t job = {.kind = WORDS_HOME, .value = page->offset};
    if (page->kind == PAGE_STUDY) job.kind = WORDS_NEXT;
    else if (page->kind == PAGE_COLLECTION) job.kind = WORDS_COLLECTION;
    else if (page->kind == PAGE_SCOPES) job.kind = WORDS_SCOPES;
    request(page, &job);
}
static void open_page(page_kind_t kind)
{
    static const char *ids[] = {"home", "study", "collection", "scopes", "settings"};
    gui_app_create_page_for_app_ext("words", ids[kind], message, (void *)(uintptr_t)kind, sizeof(page_t));
}

static void render_study(page_t *page)
{
    words_result_t *r = page->result;
    bool card = r && r->slot && r->fields[WORDS_WORD];
    const char *word = card ? r->fields[WORDS_WORD] : "暂时没有待学单词";
    const lv_font_t *font = epd_app_font_role(EPD_FONT_TEMPERATURE);
    lv_point_t size;
    lv_text_get_size(&size, word, font, 0, 0, 620, LV_TEXT_FLAG_NONE);
    if (size.y > 104) font = epd_app_font_role(EPD_FONT_TITLE);
    lv_obj_set_style_text_font(page->heading, font, 0);
    epd_app_text(page->heading, card ? r->fields[WORDS_WORD] : "暂时没有待学单词");
    epd_app_text(page->phonetic, card ? r->fields[WORDS_PHONETIC] : "");
    epd_app_text(page->body, card && page->answer ? r->fields[WORDS_TRANSLATION] : "");
    for (unsigned i = 0; i < 4; ++i) show(page->buttons[i], card && page->answer);
    show(page->buttons[4], card && !page->answer);
    show(page->buttons[5], card && page->answer);
    show(page->buttons[6], card);
    show(page->buttons[7], !card);
    if (card) button_text(page->buttons[6], r->flags & WORDS_COLLECTED ? "移出生词本" : "加入生词本");
    char text[128];
    snprintf(text, sizeof(text), "今日新词 %lu/%lu    待复习 %lu", (unsigned long)r->counts.added,
             (unsigned long)r->quota, (unsigned long)r->counts.due);
    epd_app_text(page->info, text);
}

static void render(page_t *page)
{
    words_result_t *r = page->result;
    char text[160];
    if (page->kind == PAGE_STUDY) { render_study(page); return; }
    if (page->kind == PAGE_HOME)
    {
        epd_app_text(page->heading, r->scope_name);
        snprintf(text, sizeof(text), "待复习 %lu\n\n今日新词 %lu / %lu\n\n今日复习 %lu",
                 (unsigned long)r->counts.due, (unsigned long)r->counts.added,
                 (unsigned long)r->quota, (unsigned long)r->counts.reviewed);
        epd_app_text(page->body, text);
        for (unsigned i = 0; i < 5; ++i) lv_obj_remove_state(page->buttons[i], LV_STATE_DISABLED);
        if (!r->clock_valid) epd_app_text(page->status, "请先设置系统时间");
    }
    else if (page->kind == PAGE_SETTINGS)
    {
        page->quota = r->quota;
        snprintf(text, sizeof(text), "每日新词：%lu", (unsigned long)r->quota);
        epd_app_text(page->heading, text); epd_app_text(page->body, r->scope_name);
        for (unsigned i = 0; i < 3; ++i) lv_obj_remove_state(page->buttons[i], LV_STATE_DISABLED);
    }
    else
    {
        for (unsigned i = 0; i < WORDS_MATCH_MAX; ++i)
        {
            if (i < r->matches.count) button_text(page->buttons[i], r->names[i]);
            show(page->buttons[i], i < r->matches.count);
        }
        show(page->previous, page->offset > 0); show(page->next, r->matches.more);
        if (!r->matches.count) epd_app_text(page->status, "这里空空如也~");
        else if (page->kind == PAGE_SCOPES)
        {
            snprintf(text, sizeof(text), "当前：%s", r->scope_name);
            epd_app_text(page->status, text);
        }
    }
}

static void poll(lv_timer_t *timer)
{
    page_t *page = lv_timer_get_user_data(timer);
    if (!page->started)
    {
        page->started = words_service_start();
        if (page->started) refresh(page);
        else if (++page->start_attempts >= 20)
        { epd_app_text(page->status, "应用启动失败"); lv_timer_pause(timer); }
        return;
    }
    bool failed;
    words_result_t *result = words_take(page->request, &failed);
    if (!result && !failed) return;
    lv_timer_pause(timer); page->request = 0;
    if (!result || result->error[0])
    {
        epd_app_text(page->status, result ? result->error : "内存不足");
        if (page->kind == PAGE_STUDY && (!page->result || (result && result->accepted)))
        {
            words_result_free(page->result); page->result = NULL; page->answer = false;
            for (unsigned i = 0; i < 7; ++i) show(page->buttons[i], false);
            show(page->buttons[7], true);
        }
        words_result_free(result); return;
    }
    if (page->kind == PAGE_SCOPES && result->accepted)
    {
        words_result_free(result); epd_app_text(page->status, "已切换学习范围"); refresh(page); return;
    }
    if (page->kind == PAGE_COLLECTION && page->offset && !result->matches.count)
    {
        page->offset = page->offset >= WORDS_MATCH_MAX ? page->offset - WORDS_MATCH_MAX : 0;
        words_result_free(result); refresh(page); return;
    }
    bool same = page->kind == PAGE_STUDY && page->result && result->slot &&
                page->result->slot == result->slot && page->result->token == result->token;
    if (!same) page->answer = false;
    words_result_free(page->result); page->result = result;
    epd_app_text(page->status, ""); render(page);
}

static void click(lv_event_t *event)
{
    page_t *page = lv_event_get_user_data(event);
    if (!page->started || page->request) return;
    lv_obj_t *target = lv_event_get_target_obj(event);
    if (target == page->previous || target == page->next)
    {
        if (target == page->previous) page->offset = page->offset >= WORDS_MATCH_MAX ? page->offset - WORDS_MATCH_MAX : 0;
        else page->offset += WORDS_MATCH_MAX;
        refresh(page); return;
    }
    unsigned i;
    for (i = 0; i < WORDS_MATCH_MAX && target != page->buttons[i]; ++i) {}
    if (i == WORDS_MATCH_MAX) return;
    if (page->kind == PAGE_HOME)
    {
        if (i == 0) open_page(PAGE_STUDY);
        else if (i == 1) words_open_lookup();
        else if (i == 2) open_page(PAGE_COLLECTION);
        else if (i == 3) open_page(PAGE_SCOPES);
        else open_page(PAGE_SETTINGS);
    }
    else if (page->kind == PAGE_SETTINGS)
    {
        if (i == 2) { open_page(PAGE_SCOPES); return; }
        words_job_t job = {.kind = WORDS_QUOTA};
        job.value = i == 0 ? (page->quota ? page->quota - 1 : 0) : (page->quota < 200 ? page->quota + 1 : 200);
        request(page, &job);
    }
    else if (page->kind == PAGE_STUDY)
    {
        words_result_t *r = page->result;
        if (i == 7) { refresh(page); return; }
        if (!r || !r->slot) return;
        if (i == 4) { page->answer = true; render_study(page); return; }
        if (i == 5)
        {
            page->child = (words_job_t){.kind = WORDS_DETAIL, .slot = r->slot};
            words_open_detail(&page->child); return;
        }
        words_job_t job = {.slot = r->slot, .token = r->token};
        if (i < 4)
        {
            if (!page->answer) return;
            job.kind = WORDS_RATE; job.value = i + 1;
        }
        else { job.kind = WORDS_COLLECT; job.value = !(r->flags & WORDS_COLLECTED); }
        request(page, &job);
    }
    else if (page->kind == PAGE_COLLECTION && page->result && i < page->result->matches.count)
    {
        page->child = (words_job_t){.kind = WORDS_DETAIL, .slot = page->result->matches.entry[i]};
        words_open_detail(&page->child);
    }
    else if (page->kind == PAGE_SCOPES && page->result && i < page->result->matches.count)
    {
        words_job_t job = {.kind = WORDS_SCOPE_SELECT};
        strcpy(job.query, page->result->scope_ids[i]);
        memcpy(job.identity, page->result->identity, sizeof(job.identity));
        epd_app_text(page->status, "正在读取学习范围"); request(page, &job);
    }
}

static void leave(lv_event_t *event)
{
    page_t *page = lv_event_get_user_data(event);
    if (!page->root || words_service_flush()) gui_app_goback();
    else epd_app_text(page->status, "保存未完成，请重试");
}

static void build(page_t *page)
{
    static const char *titles[] = {"单词", "学习", "生词本", "学习范围", "单词设置"};
    page->screen = words_prepare_back(titles[page->kind], leave, page);
    page->status = epd_app_label(page->screen, "", 32, 154, 620, epd_app_font_role(EPD_FONT_SMALL));
    if (page->kind == PAGE_HOME)
    {
        page->heading = epd_app_label(page->screen, "", 32, 222, 620, epd_app_font_role(EPD_FONT_TITLE));
        page->body = epd_app_label(page->screen, "", 32, 310, 620, epd_app_font());
        lv_obj_set_height(page->body, 260); lv_label_set_long_mode(page->body, LV_LABEL_LONG_WRAP);
        const char *labels[] = {"开始学习", "查词", "生词本", "学习范围", "设置"};
        for (unsigned i = 0; i < 5; ++i)
        {
            page->buttons[i] = epd_app_button(page->screen, 32, 620 + i * 112, 620, 88, labels[i], click, page);
            lv_obj_add_state(page->buttons[i], LV_STATE_DISABLED);
        }
    }
    else if (page->kind == PAGE_SETTINGS)
    {
        page->heading = epd_app_label(page->screen, "", 32, 236, 620, epd_app_font_role(EPD_FONT_TITLE));
        page->buttons[0] = epd_app_button(page->screen, 32, 338, 294, 88, "减少", click, page);
        page->buttons[1] = epd_app_button(page->screen, 358, 338, 294, 88, "增加", click, page);
        epd_app_label(page->screen, "当前学习范围", 32, 502, 620, epd_app_font());
        page->body = epd_app_label(page->screen, "", 32, 572, 620, epd_app_font());
        page->buttons[2] = epd_app_button(page->screen, 32, 672, 620, 88, "选择学习范围", click, page);
        for (unsigned i = 0; i < 3; ++i) lv_obj_add_state(page->buttons[i], LV_STATE_DISABLED);
    }
    else if (page->kind == PAGE_STUDY)
    {
        page->heading = epd_app_label(page->screen, "", 32, 248, 620, epd_app_font_role(EPD_FONT_TEMPERATURE));
        lv_obj_set_height(page->heading, 104);
        page->phonetic = epd_app_label(page->screen, "", 32, 374, 620, epd_app_font());
        page->body = epd_app_label(page->screen, "", 32, 470, 620, epd_app_font());
        lv_obj_set_height(page->body, 422); lv_label_set_long_mode(page->body, LV_LABEL_LONG_DOT);
        page->info = epd_app_label(page->screen, "", 32, 1154, 620, epd_app_font_role(EPD_FONT_SMALL));
        const char *ratings[] = {"忘记", "困难", "记住", "熟练"};
        for (unsigned i = 0; i < 4; ++i)
            page->buttons[i] = epd_app_button(page->screen, 32 + i * 158, 1040, 146, 86, ratings[i], click, page);
        page->buttons[4] = epd_app_button(page->screen, 32, 1040, 620, 86, "查看答案", click, page);
        page->buttons[5] = epd_app_button(page->screen, 32, 934, 294, 80, "完整释义", click, page);
        page->buttons[6] = epd_app_button(page->screen, 358, 934, 294, 80, "加入生词本", click, page);
        page->buttons[7] = epd_app_button(page->screen, 32, 1040, 620, 86, "继续学习", click, page);
        for (unsigned i = 0; i < WORDS_MATCH_MAX; ++i) show(page->buttons[i], false);
    }
    else
    {
        for (unsigned i = 0; i < WORDS_MATCH_MAX; ++i)
        {
            page->buttons[i] = epd_app_button(page->screen, 32, 210 + i * 108, 620, 88, "", click, page);
            show(page->buttons[i], false);
        }
        page->previous = epd_app_button(page->screen, 32, 1100, 294, 76, "上一页", click, page);
        page->next = epd_app_button(page->screen, 358, 1100, 294, 76, "下一页", click, page);
        show(page->previous, false); show(page->next, false);
    }
    page->timer = lv_timer_create(poll, 100, page); lv_timer_pause(page->timer);
}

static void message(gui_app_msg_type_t msg, void *parameter)
{
    (void)parameter;
    page_t *page = gui_app_this_page_memory();
    if (msg == GUI_APP_MSG_ONSTART)
    {
        uintptr_t value = (uintptr_t)gui_app_this_page_userdata();
        page->root = (value & 0x100u) != 0;
        page->kind = (page_kind_t)(value & 0xffu);
        build(page); page->started = !page->root || words_service_start();
    }
    else if (msg == GUI_APP_MSG_ONRESUME)
    {
        epd_app_header_refresh(page->screen);
        if (!page->started) lv_timer_resume(page->timer);
        else refresh(page);
    }
    else if (msg == GUI_APP_MSG_ONPAUSE)
    {
        lv_timer_pause(page->timer); words_cancel(page->request); page->request = 0;
    }
    else if (msg == GUI_APP_MSG_ONSTOP)
    {
        lv_timer_delete(page->timer); words_result_free(page->result);
        if (page->root) words_service_stop();
    }
}

int app_main(intent_t intent)
{
    const char *entry = intent_get_string(intent, "page");
    uintptr_t root = (entry && !strcmp(entry, "settings") ? PAGE_SETTINGS : PAGE_HOME) | 0x100u;
    return gui_app_regist_msg_handler_ext("words", message, (void *)root, sizeof(page_t));
}
