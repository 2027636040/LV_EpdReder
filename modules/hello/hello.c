#include "platform/epd_app.h"

EPD_APP_DEFINE("hello");

/* This task outlives pages. It owns no LVGL objects or external callbacks. */
static void background_run(epd_service_t *service)
{
    while (!epd_service_cancelled(service))
    {
        uint32_t events = epd_service_wait(service, RT_WAITING_FOREVER);
        if (events & EPD_SERVICE_STOP) break;
        if (events & EPD_SERVICE_NETWORK)
        {
            network_snapshot_t snapshot;
            network_snapshot(&snapshot);
            rt_kprintf("[hello] network %s, generation=%u\n",
                       snapshot.ready ? "online" : "offline", (unsigned)snapshot.generation);
        }
    }
}

const epd_background_t epd_app_background = {background_run, 2048, 24, true};

typedef struct
{
    lv_obj_t *count_label;
    unsigned count;
} hello_page_t;

static lv_obj_t *screen_prepare(const char *title)
{
    gui_app_close_anim();
    lv_obj_t *screen = lv_screen_active();
    lv_obj_remove_style_all(screen);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);
    lv_obj_set_style_text_font(screen, epd_app_font(), 0);
    epd_app_back_button(screen);
    lv_obj_t *label = lv_label_create(screen);
    lv_label_set_text(label, title);
    lv_obj_set_pos(label, 104, 103);
    return screen;
}

static void detail_message(gui_app_msg_type_t message, void *parameter)
{
    (void)parameter;
    if (message != GUI_APP_MSG_ONSTART) return;
    lv_obj_t *screen = screen_prepare("示例子页面");
    lv_obj_t *label = lv_label_create(screen);
    lv_label_set_text(label, "返回后，点击次数保持不变");
    lv_obj_set_pos(label, 80, 330);
}

static void detail_clicked(lv_event_t *event)
{
    (void)event;
    gui_app_create_page_for_app_ext("hello", "detail", detail_message, NULL, 0);
}

static void count_clicked(lv_event_t *event)
{
    hello_page_t *page = lv_event_get_user_data(event);
    lv_label_set_text_fmt(page->count_label, "点击次数：%u", ++page->count);
}

static void button_create(lv_obj_t *screen, int y, const char *text,
                          lv_event_cb_t callback, void *data)
{
    lv_obj_t *button = epd_app_button(screen, 80, y, 524, 100, text, callback, data);
    lv_obj_set_style_radius(button, 0, 0);
    lv_obj_set_style_border_width(button, 2, 0);
}

static void root_message(gui_app_msg_type_t message, void *parameter)
{
    (void)parameter;
    if (message != GUI_APP_MSG_ONSTART) return;
    hello_page_t *page = gui_app_this_page_memory();
    lv_obj_t *screen = screen_prepare("本地动态应用");
    page->count_label = lv_label_create(screen);
    lv_label_set_text(page->count_label, "点击次数：0");
    lv_obj_set_pos(page->count_label, 80, 300);
    button_create(screen, 430, "点击计数", count_clicked, page);
    button_create(screen, 570, "进入子页面", detail_clicked, NULL);
}

/* app_fwk calls this entry on its UI thread, not in module_init. */
int app_main(intent_t intent)
{
    (void)intent;
    return gui_app_regist_msg_handler_ext("hello", root_message, NULL, sizeof(hello_page_t));
}
