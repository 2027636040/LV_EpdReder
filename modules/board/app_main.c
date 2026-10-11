#include "platform/epd_app.h"
#include "board_model.h"
#include "board_store.h"
#include <string.h>

EPD_APP_DEFINE("board");

typedef struct
{
    board_model_t *model;
    lv_obj_t *root;
    lv_obj_t **bodies;
    lv_timer_t *timer;
    uint32_t revision;
    bool foreground, observed;
} board_page_t;

static void back(lv_event_t *event)
{
    (void)event;
    gui_app_goback();
}

static bool draw(const board_command_t *c, void *context)
{
    board_page_t *page = context;
    lv_obj_t *object;
    if (c->kind == BOARD_BACK)
    {
        object = epd_app_button(page->root, c->x, c->y, c->width, c->height, c->text, back, NULL);
        epd_app_input_back(page->root, object);
    }
    else if (c->kind == BOARD_PANEL)
        object = epd_app_panel(page->root, c->x, c->y, c->width, c->height);
    else
    {
        object = epd_app_label(page->root, c->text, c->x, c->y, c->width,
                              epd_app_font_role(c->kind == BOARD_TITLE ? EPD_FONT_TITLE : EPD_FONT_BODY));
        lv_obj_set_size(object, c->width, c->height);
        lv_label_set_long_mode(object, c->kind == BOARD_BODY ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_CLIP);
        if (c->kind == BOARD_BODY) page->bodies[c->card] = object;
    }
    if (c->kind == BOARD_BACK || c->kind == BOARD_PANEL)
    {
        lv_obj_set_style_radius(object, 0, 0);
        lv_obj_set_style_border_color(object, lv_color_black(), 0);
        lv_obj_set_style_border_width(object, 1, 0);
    }
    return true;
}

static void refresh(board_page_t *page)
{
    if (!page->foreground || !page->model || !page->bodies) return;
    uint32_t revision = epd_app_summary_revision();
    if (page->observed && page->revision == revision) return;
    size_t i = 0;
    for (const board_card_t *card = page->model->cards; card; card = card->next, ++i)
    {
        if (!card->summary) continue;
        char text[96] = {0};
        bool found = epd_app_summary_read(card->summary, text, sizeof(text));
        text[sizeof(text) - 1] = 0;
        epd_app_text(page->bodies[i], found && text[0] ? text : card->empty);
    }
    /* A publication during this pass causes a new read on the next UI tick. */
    page->revision = revision;
    page->observed = true;
}

static void poll(lv_timer_t *timer)
{
    refresh(lv_timer_get_user_data(timer));
}

static void release(board_page_t *page)
{
    page->foreground = false;
    if (page->timer) { lv_timer_delete(page->timer); page->timer = NULL; }
    if (page->root) { lv_obj_delete(page->root); page->root = NULL; }
    epd_app_free(page->bodies);
    page->bodies = NULL;
    board_model_free(page->model);
    page->model = NULL;
}

static lv_obj_t *root_create(lv_obj_t *screen, int32_t width, int32_t height)
{
    lv_obj_t *root = epd_app_panel(screen, 0, 0, width, height);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    epd_app_input_group(root);
    return root;
}

static void start(board_page_t *page)
{
    gui_app_close_anim();
    lv_obj_t *screen = lv_screen_active();
    epd_app_object_init(screen);
    int32_t width = lv_obj_get_width(screen), height = lv_obj_get_height(screen);
    const char *message;
    board_result_t result = board_store_load(&page->model, &message);
    if (result == BOARD_OK)
    {
        result = board_layout(page->model, width, height, NULL, NULL);
        if (result != BOARD_OK) message = "看板布局超出屏幕范围";
    }
    if (result == BOARD_OK && page->model->count)
    {
        if (page->model->count <= SIZE_MAX / sizeof(*page->bodies))
            page->bodies = epd_app_alloc(page->model->count * sizeof(*page->bodies), EPD_APP_PSRAM);
        if (!page->bodies) { result = BOARD_MEMORY; message = "内存不足，无法显示看板"; }
    }
    if (result == BOARD_OK)
    {
        bool summaries = false;
        for (const board_card_t *c = page->model->cards; c; c = c->next) summaries |= c->summary != NULL;
        if (summaries)
        {
            page->timer = lv_timer_create(poll, 500, page);
            if (page->timer) lv_timer_pause(page->timer);
            else { result = BOARD_MEMORY; message = "无法创建摘要刷新定时器"; }
        }
    }
    if (result == BOARD_OK)
    {
        page->root = root_create(screen, width, height);
        board_layout(page->model, width, height, draw, page);
    }
    else
    {
        release(page);
        page->root = root_create(screen, width, height);
        lv_obj_t *button = epd_app_button(page->root, 24, 24, 100, 64, "返回", back, NULL);
        epd_app_input_back(page->root, button);
        epd_app_label(page->root, message, 24, 120, width - 48, epd_app_font());
    }
}

static void message(gui_app_msg_type_t type, void *parameter)
{
    (void)parameter;
    board_page_t *page = gui_app_this_page_memory();
    switch (type)
    {
    case GUI_APP_MSG_ONSTART:
        memset(page, 0, sizeof(*page));
        start(page);
        break;
    case GUI_APP_MSG_ONRESUME:
        page->foreground = true;
        page->observed = false;
        refresh(page);
        if (page->timer) lv_timer_resume(page->timer);
        break;
    case GUI_APP_MSG_ONPAUSE:
        page->foreground = false;
        if (page->timer) lv_timer_pause(page->timer);
        break;
    case GUI_APP_MSG_ONSTOP:
        release(page);
        break;
    default: break;
    }
}

int app_main(intent_t intent)
{
    (void)intent;
    return gui_app_regist_msg_handler_ext("board", message, NULL, sizeof(board_page_t));
}
