#include "platform/epd_app.h"

EPD_APP_DEFINE("hello");

static void page_message(gui_app_msg_type_t message, void *parameter);

static void next_page(lv_event_t *event)
{
    (void)event;
    gui_app_create_page_for_app_ext("hello", "second", page_message, (void *)1, 0);
}

static void page_message(gui_app_msg_type_t message, void *parameter)
{
    (void)parameter;
    if (message == GUI_APP_MSG_ONSTART)
    {
        lv_obj_t *screen = lv_screen_active();
        gui_app_close_anim();
        epd_app_object_init(screen);
        epd_app_input_group(screen);
        epd_app_header(screen);
        bool second = gui_app_this_page_userdata() != NULL;
        epd_app_label(screen, second ? "Second page. Press Back to return."
                                     : "Hello from an independent app!",
                      32, 160, 580, epd_app_font());
        if (!second)
        {
            epd_app_button(screen, 32, 260, 400, 72, "Open second page", next_page, NULL);
        }
        epd_app_back_button(screen);
    }
    else if (message == GUI_APP_MSG_ONRESUME)
    {
        epd_app_header_refresh(lv_screen_active());
    }
}

int app_main(intent_t intent)
{
    (void)intent;
    return gui_app_regist_msg_handler("hello", page_message);
}
