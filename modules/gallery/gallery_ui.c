#include "gallery.h"
#include "src/draw/lv_image_decoder_private.h"
#include <string.h>
#include <stdio.h>

EPD_APP_DEFINE("gallery");

typedef struct
{
    lv_obj_t *screen, *breadcrumb, *empty, *rows[GALLERY_ROWS], *labels[GALLERY_ROWS];
    lv_obj_t *previous, *next;
    lv_timer_t *timer;
    char path[STORAGE_PATH_MAX], selected[STORAGE_PATH_MAX];
    gallery_entry_t entries[GALLERY_ROWS];
    unsigned first, count, selected_row, depth, offsets[32], start_attempts;
    uint32_t request;
    bool started;
} browser_t;

typedef struct
{
    lv_obj_t *screen, *image, *status;
    lv_timer_t *timer;
    char path[STORAGE_PATH_MAX];
    uint32_t request;
    gallery_result_t *displayed;
} viewer_t;

static void browser_scan(browser_t *page);
static void viewer_message(gui_app_msg_type_t message, void *parameter);

static void visible(lv_obj_t *object, bool show)
{
    if (show) lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *prepare(const char *title, lv_event_cb_t back, void *data)
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

static void browser_back(lv_event_t *event)
{
    browser_t *page = lv_event_get_user_data(event);
    if (!page->path[0]) { gui_app_goback(); return; }
    char *slash = strrchr(page->path, '/');
    if (slash) *slash = 0;
    page->first = page->depth ? page->offsets[--page->depth] : 0;
    page->selected_row = 0;
    browser_scan(page);
}

static void row_clicked(lv_event_t *event)
{
    browser_t *page = lv_event_get_user_data(event);
    unsigned index;
    for (index = 0; index < page->count; ++index)
        if (page->rows[index] == lv_event_get_target_obj(event)) break;
    if (index == page->count || !page->started) return;
    page->selected_row = index;
    if (!page->path[0])
    {
        if (!storage_available(index)) { epd_app_text(page->empty, "存储不可用"); visible(page->empty, true); return; }
        page->offsets[page->depth++] = 0;
        snprintf(page->path, sizeof(page->path), "%s", storage_root(index));
        page->first = 0;
        browser_scan(page);
        return;
    }
    int n = snprintf(page->selected, sizeof(page->selected), "%s/%s", page->path, page->entries[index].name);
    if (n <= 0 || (size_t)n >= sizeof(page->selected))
    { epd_app_text(page->empty, "路径过长"); visible(page->empty, true); return; }
    if (page->entries[index].directory)
    {
        if (page->depth == 32) { epd_app_text(page->empty, "目录层级过深"); visible(page->empty, true); return; }
        page->offsets[page->depth++] = page->first;
        strcpy(page->path, page->selected);
        page->first = page->selected_row = 0;
        browser_scan(page);
    }
    else gui_app_create_page_for_app_ext("gallery", "picture", viewer_message, page->selected, sizeof(viewer_t));
}

static void list_page_clicked(lv_event_t *event)
{
    browser_t *page = lv_event_get_user_data(event);
    if (lv_event_get_target_obj(event) == page->previous)
        page->first = page->first >= GALLERY_ROWS ? page->first - GALLERY_ROWS : 0;
    else page->first += GALLERY_ROWS;
    page->selected_row = 0;
    browser_scan(page);
}

static void browser_scan(browser_t *page)
{
    gallery_cancel_request(page->request);
    page->request = 0;
    for (unsigned i = 0; i < GALLERY_ROWS; ++i) visible(page->rows[i], false);
    visible(page->previous, false);
    visible(page->next, false);
    visible(page->empty, false);
    epd_app_text(page->breadcrumb, page->path);
    if (!page->path[0])
    {
        page->count = STORAGE_COUNT;
        epd_app_text(page->labels[STORAGE_FLASH], "内部存储空间");
        epd_app_text(page->labels[STORAGE_SD], "TF 卡");
        for (unsigned i = 0; i < STORAGE_COUNT; ++i) visible(page->rows[i], true);
        lv_timer_pause(page->timer);
        return;
    }
    page->count = 0;
    epd_app_text(page->empty, "正在读取");
    visible(page->empty, true);
    page->request = gallery_submit(GALLERY_SCAN, page->path, page->first);
    lv_timer_resume(page->timer);
}

static void browser_poll(lv_timer_t *timer)
{
    browser_t *page = lv_timer_get_user_data(timer);
    if (!page->started)
    {
        page->started = gallery_start();
        if (page->started) browser_scan(page);
        else if (++page->start_attempts >= 20)
        {
            epd_app_text(page->empty, "图库服务启动失败");
            visible(page->empty, true);
            lv_timer_pause(timer);
        }
        return;
    }
    if (!page->request) return;
    gallery_result_t *result = gallery_take(page->request);
    if (!result && !gallery_failed(page->request)) return;
    lv_timer_pause(timer);
    if (!result)
    {
        epd_app_text(page->empty, "图片内存不足");
        visible(page->empty, true);
        return;
    }
    page->count = result->count;
    memcpy(page->entries, result->entries, sizeof(page->entries));
    for (unsigned i = 0; i < GALLERY_ROWS; ++i)
    {
        visible(page->rows[i], i < page->count);
        if (i < page->count)
        {
            char name[280];
            snprintf(name, sizeof(name), "%s%s", page->entries[i].name,
                     page->entries[i].directory ? "  ›" : "");
            epd_app_text(page->labels[i], name);
        }
    }
    epd_app_text(page->empty, result->error[0] ? result->error : "这里空空如也~");
    visible(page->empty, result->error[0] || !page->count);
    visible(page->previous, page->first != 0);
    visible(page->next, result->more);
    if (page->count) lv_group_focus_obj(page->rows[page->selected_row < page->count ? page->selected_row : 0]);
    gallery_result_free(result);
}

static void browser_message(gui_app_msg_type_t message, void *parameter)
{
    (void)parameter;
    browser_t *page = gui_app_this_page_memory();
    if (message == GUI_APP_MSG_ONSTART)
    {
        page->screen = prepare("图库", browser_back, page);
        page->breadcrumb = epd_app_label(page->screen, "", 32, 170, 620, epd_app_font_role(EPD_FONT_SMALL));
        lv_label_set_long_mode(page->breadcrumb, LV_LABEL_LONG_DOT);
        page->empty = epd_app_centered_label(page->screen, "", 32, 520, 620, epd_app_font());
        for (unsigned i = 0; i < GALLERY_ROWS; ++i)
        {
            page->rows[i] = epd_app_button(page->screen, 32, 224 + i * 104, 620, 92, NULL, row_clicked, page);
            page->labels[i] = epd_app_label(page->rows[i], "", 20, 25, 572, epd_app_font());
            lv_label_set_long_mode(page->labels[i], LV_LABEL_LONG_DOT);
            visible(page->rows[i], false);
        }
        page->previous = epd_app_button(page->screen, 32, 1100, 294, 76, "上一页", list_page_clicked, page);
        page->next = epd_app_button(page->screen, 358, 1100, 294, 76, "下一页", list_page_clicked, page);
        visible(page->previous, false);
        visible(page->next, false);
        page->timer = lv_timer_create(browser_poll, 100, page);
        lv_timer_pause(page->timer);
        page->started = gallery_start();
    }
    else if (message == GUI_APP_MSG_ONRESUME)
    {
        epd_app_header_refresh(page->screen);
        if (page->started && page->path[0])
        {
            page->request = gallery_submit(GALLERY_SCAN, page->path, page->first);
            lv_timer_resume(page->timer);
        }
        else if (page->started) browser_scan(page);
        else lv_timer_resume(page->timer);
    }
    else if (message == GUI_APP_MSG_ONPAUSE)
    {
        lv_timer_pause(page->timer);
        gallery_cancel_request(page->request);
    }
    else if (message == GUI_APP_MSG_ONSTOP)
    {
        lv_timer_delete(page->timer);
        gallery_stop();
    }
}

static void release_image(viewer_t *page)
{
    if (!page->displayed) return;
    lv_image_set_src(page->image, NULL);
    lv_draw_wait_for_finish();
    lv_image_cache_drop(&page->displayed->image);
    lv_image_header_cache_drop(&page->displayed->image);
    gallery_result_free(page->displayed);
    page->displayed = NULL;
}

static void viewer_request(viewer_t *page)
{
    page->request = gallery_submit(GALLERY_IMAGE, page->path, 0);
    if (!page->displayed) epd_app_text(page->status, "正在读取");
    lv_timer_resume(page->timer);
}

static void viewer_poll(lv_timer_t *timer)
{
    viewer_t *page = lv_timer_get_user_data(timer);
    gallery_result_t *result = gallery_take(page->request);
    if (!result && !gallery_failed(page->request)) return;
    lv_timer_pause(timer);
    if (!result) { epd_app_text(page->status, "图片内存不足"); return; }
    if (result->error[0])
    {
        epd_app_text(page->status, result->error);
        gallery_result_free(result);
        return;
    }
    release_image(page);
    page->displayed = result;
    epd_app_text(page->status, "");
    lv_obj_set_pos(page->image, 32 + (GALLERY_WIDTH - result->image.header.w) / 2,
                   218 + (GALLERY_HEIGHT - result->image.header.h) / 2);
    lv_image_set_src(page->image, &result->image);
}

static void viewer_message(gui_app_msg_type_t message, void *parameter)
{
    (void)parameter;
    viewer_t *page = gui_app_this_page_memory();
    if (message == GUI_APP_MSG_ONSTART)
    {
        snprintf(page->path, sizeof(page->path), "%s", (const char *)gui_app_this_page_userdata());
        page->screen = prepare("图库", NULL, NULL);
        const char *name = strrchr(page->path, '/');
        lv_obj_t *label = epd_app_label(page->screen, name ? name + 1 : page->path,
                                       32, 170, 620, epd_app_font_role(EPD_FONT_SMALL));
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        page->image = lv_image_create(page->screen);
        page->status = epd_app_centered_label(page->screen, "正在读取", 32, 600, 620, epd_app_font());
        page->timer = lv_timer_create(viewer_poll, 100, page);
        lv_timer_pause(page->timer);
    }
    else if (message == GUI_APP_MSG_ONRESUME)
    {
        epd_app_header_refresh(page->screen);
        if (!page->displayed) viewer_request(page);
    }
    else if (message == GUI_APP_MSG_ONPAUSE)
    {
        lv_timer_pause(page->timer);
        gallery_cancel_request(page->request);
    }
    else if (message == GUI_APP_MSG_ONSTOP)
    {
        lv_timer_delete(page->timer);
        gallery_cancel_request(page->request);
        release_image(page);
    }
}

int app_main(intent_t intent)
{
    (void)intent;
    return gui_app_regist_msg_handler_ext("gallery", browser_message, NULL, sizeof(browser_t));
}
