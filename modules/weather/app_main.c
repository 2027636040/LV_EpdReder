#include "weather_ui.h"
#include "storage.h"

EPD_APP_DEFINE("weather");
weather_page_t *current;

static const char *const page_names[] = {"root", "settings", "city", "city_input", "city_history"};
static void page_message(gui_app_msg_type_t message, void *parameter);
static void action_clicked(lv_event_t *event);

lv_obj_t *page_title_create(lv_obj_t *screen, const char *text, const lv_font_t *font)
{
    return label_create(screen, text, PAGE_TITLE_X, PAGE_TITLE_Y, WIDTH - MARGIN - PAGE_TITLE_X, font);
}

lv_obj_t *button_create(lv_obj_t *parent, int x, int y, int w, int h, const char *text, int action)
{
    lv_obj_t *button = epd_app_button(parent, x, y, w, h, text,
                                     action_clicked, (void *)(intptr_t)action);
    return button;
}

void popup_close(void)
{
    if (!current->popup) return;
    lv_obj_delete(current->popup);
    current->popup = current->popup_text = current->popup_button = NULL;
    current->weather_popup_phase = 0;
    current->group = current->page_group;
    if (current->previous_focus) lv_group_focus_obj(current->previous_focus);
}

void popup_open(const char *text, const char *detail)
{
    if (current->popup) return;
    current->previous_focus = lv_group_get_focused(current->group);
    current->popup = panel_create(current->screen, 0, 0, WIDTH, HEIGHT);
    current->group = epd_app_input_group(current->popup);
    lv_obj_add_flag(current->popup, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(current->popup, 0, 0);
    lv_obj_set_style_border_width(current->popup, 0, 0);
    lv_obj_set_style_bg_opa(current->popup, LV_OPA_TRANSP, 0);
    lv_obj_t *dialog = panel_create(current->popup, 54, 438, 576, 282);
    lv_obj_set_style_border_color(dialog, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_width(dialog, 2, 0);
    current->popup_text = centered_label(dialog, text, 24, detail ? 44 : 74, 528, ui_font_body());
    if (detail) centered_label(dialog, detail, 24, 102, 528, ui_font_small());
    current->popup_button = button_create(dialog, 108, 184, 360, 64, "确定", POPUP_CLOSE);
    epd_app_input_back(current->popup, current->popup_button);
    lv_group_focus_obj(current->popup_button);
}

void weather_page_open(unsigned page)
{
    if (page >= sizeof(page_names) / sizeof(page_names[0])) return;
    if (gui_app_create_page_for_app_ext("weather", page_names[page], page_message,
             (void *)(uintptr_t)page, sizeof(weather_page_t)) != RT_EOK)
        popup_open("页面打开失败，请重试", NULL);
}

void weather_settings_return(void)
{
    /* A settings Intent gives this application a settings root page. */
    if (gui_app_goback_to_page("root") != RT_EOK) popup_open("返回失败，请重试", NULL);
}

static void action_clicked(lv_event_t *event)
{
    if (!current || !current->foreground || storage_changing() || current->weather_popup_phase ||
        lv_obj_get_screen(lv_event_get_target_obj(event)) != current->screen) return;
    int action = (int)(intptr_t)lv_event_get_user_data(event);
    lv_group_focus_obj(lv_event_get_target_obj(event));
    if (action == POPUP_CLOSE) popup_close();
    else if (action == NAV_BACK)
    {
        if (current->popup) popup_close();
        else gui_app_goback();
    }
    else if (action == WEATHER_SYNC) weather_action_start(WEATHER_JOB_SYNC);
    else if (action == WEATHER_IMPORT) weather_action_start(WEATHER_JOB_IMPORT);
    else if (city_action(action)) return;
    else if (city_history_action(action)) return;
    else if (action >= 0) weather_page_open((unsigned)action);
}

static void page_tick(lv_timer_t *timer)
{
    weather_page_t *page = lv_timer_get_user_data(timer);
    if (page == current && page->foreground) weather_ui_process();
}

static void page_message(gui_app_msg_type_t message, void *parameter)
{
    (void)parameter;
    weather_page_t *page = gui_app_this_page_memory();
    weather_page_t *previous = current;
    current = page;
    switch (message)
    {
    case GUI_APP_MSG_ONSTART:
    {
        gui_app_close_anim();
        page->id = (unsigned)(uintptr_t)gui_app_this_page_userdata();
        page->screen = lv_screen_active();
        page->page_group = page->group = epd_app_input_group(page->screen);
        epd_obj_init(page->screen);
        lv_obj_set_size(page->screen, WIDTH, HEIGHT);
        lv_obj_set_style_bg_color(page->screen, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(page->screen, LV_OPA_COVER, 0);
        lv_obj_set_style_text_font(page->screen, ui_font_body(), 0);
        epd_app_header(page->screen);
        switch (page->id)
        {
        case UI_PAGE_WEATHER: weather_create(page->screen); break;
        case UI_PAGE_WEATHER_SETTINGS: weather_settings_create(page->screen); break;
        case UI_PAGE_CITY: city_create(page->screen); break;
        case UI_PAGE_CITY_INPUT: city_input_create(page->screen); break;
        case UI_PAGE_CITY_HISTORY: city_history_create(page->screen); break;
        }
        epd_app_back_button_cb(page->screen, action_clicked, (void *)(intptr_t)NAV_BACK);
        page->timer = lv_timer_create(page_tick, 250, page);
        lv_timer_pause(page->timer);
        current = previous;
        break;
    }
    case GUI_APP_MSG_ONRESUME:
        page->foreground = true;
        epd_app_header_refresh(page->screen);
        weather_data_update();
        if (page->id == UI_PAGE_WEATHER) weather_resume();
        else if (page->id == UI_PAGE_WEATHER_SETTINGS) weather_settings_render();
        else if (page->id == UI_PAGE_CITY_HISTORY) city_history_resume();
        lv_timer_resume(page->timer);
        weather_ui_process();
        break;
    case GUI_APP_MSG_ONPAUSE:
        lv_timer_pause(page->timer);
        if (page->id == UI_PAGE_WEATHER) weather_pause();
        if (page->id == UI_PAGE_CITY_INPUT) city_input_pause();
        page->foreground = false;
        current = NULL;
        break;
    case GUI_APP_MSG_ONSTOP:
        lv_timer_delete(page->timer);
        if (page->id == UI_PAGE_CITY_HISTORY) city_history_stop();
        current = previous == page ? NULL : previous;
        /* Screen deletion releases input groups; app_fwk owns page memory. */
        break;
    default:
        current = previous;
        break;
    }
}

int app_main(intent_t intent)
{
    if (!weather_service_ready()) return -RT_ENOMEM;
    const char *entry = intent_get_string(intent, "page");
    unsigned root = entry && !strcmp(entry, "settings") ? UI_PAGE_WEATHER_SETTINGS : UI_PAGE_WEATHER;
    return gui_app_regist_msg_handler_ext("weather", page_message,
                 (void *)(uintptr_t)root, sizeof(weather_page_t));
}
