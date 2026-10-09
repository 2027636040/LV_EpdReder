#include "epd_app.h"
#include "epd_input.h"
#include "ui_font.h"
#include "ui_reader_font_cache.h"
#include <rtdevice.h>
#include "ui_internal.h"
#include "icons/ui_icons.h"
#include <rtm.h>
#include "src/misc/lv_text_private.h"
#include "src/draw/lv_image_decoder_private.h"

static void back_clicked(lv_event_t *event)
{
    (void)event;
    gui_app_goback();
}

const lv_font_t *epd_app_font(void)
{
    return ui_font_body();
}
RTM_EXPORT(epd_app_font);

const void *epd_app_builtin_font_data(size_t *size)
{
    return ui_font_builtin_data(size);
}
RTM_EXPORT(epd_app_builtin_font_data);

const lv_font_t *epd_app_font_role(epd_font_role_t role)
{
    switch (role)
    {
    case EPD_FONT_SMALL: return ui_font_small();
    case EPD_FONT_TITLE: return ui_font_title();
    case EPD_FONT_CAPTION: return ui_font_caption();
    case EPD_FONT_TEMPERATURE: return ui_font_temperature();
    default: return ui_font_body();
    }
}
void epd_app_object_init(lv_obj_t *obj) { epd_obj_init(obj); }
lv_obj_t *epd_app_label(lv_obj_t *p, const char *t, int x, int y, int w, const lv_font_t *f)
{ return label_create(p, t, x, y, w, f); }
lv_obj_t *epd_app_centered_label(lv_obj_t *p, const char *t, int x, int y, int w, const lv_font_t *f)
{ return centered_label(p, t, x, y, w, f); }
lv_obj_t *epd_app_panel(lv_obj_t *p, int x, int y, int w, int h) { return panel_create(p, x, y, w, h); }
lv_obj_t *epd_app_icon(lv_obj_t *p, const lv_image_dsc_t *s, int x, int y) { return icon_create(p, s, x, y); }
void epd_app_text(lv_obj_t *label, const char *text) { text_update(label, text); }
bool epd_app_refresh_done(void) { return lv_refreshing_done(); }
void epd_app_clean_draw_buffer(lv_draw_buf_t *buffer)
{
#if defined(PSRAM_CACHE_WB)
    if (buffer) mpu_dcache_clean(buffer->data, buffer->data_size);
#else
    (void)buffer;
#endif
}
lv_obj_t *epd_app_button(lv_obj_t *parent, int x, int y, int width, int height,
                         const char *text, lv_event_cb_t callback, void *data)
{
    lv_obj_t *button = panel_create(parent, x, y, width, height);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_color(button, lv_color_black(), LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(button, lv_color_black(), LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(button, 3, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(button, -4, LV_STATE_FOCUSED);
    if (callback) lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, data);
    epd_input_add_object(button);
    if (text)
    {
        lv_obj_t *label = centered_label(button, text, 0, 0, width - 24, ui_font_body());
        lv_obj_center(label);
    }
    return button;
}
RTM_EXPORT(epd_app_font_role);
RTM_EXPORT(epd_app_object_init);
RTM_EXPORT(epd_app_label);
RTM_EXPORT(epd_app_centered_label);
RTM_EXPORT(epd_app_panel);
RTM_EXPORT(epd_app_icon);
RTM_EXPORT(epd_app_text);
RTM_EXPORT(epd_app_button);
RTM_EXPORT(epd_app_header);
RTM_EXPORT(epd_app_header_refresh);
RTM_EXPORT(epd_app_refresh_done);
RTM_EXPORT(epd_app_clean_draw_buffer);
RTM_EXPORT(intent_get_string);

lv_obj_t *epd_app_back_button_cb(lv_obj_t *screen, lv_event_cb_t callback, void *data)
{
    lv_obj_t *button = lv_obj_create(screen);
    lv_obj_remove_style_all(button);
    lv_obj_set_pos(button, 32, 94);
    lv_obj_set_size(button, 56, 56);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, data);
    lv_obj_set_style_outline_color(button, lv_color_black(), LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(button, 3, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(button, -4, LV_STATE_FOCUSED);
    epd_input_add_object(button);
    epd_app_input_back(screen, button);
    lv_obj_t *icon = lv_image_create(button);
    lv_image_set_src(icon, &ui_icon_back);
    lv_obj_center(icon);
    return button;
}
RTM_EXPORT(epd_app_back_button_cb);
lv_obj_t *epd_app_back_button(lv_obj_t *screen)
{
    return epd_app_back_button_cb(screen, back_clicked, NULL);
}
RTM_EXPORT(epd_app_back_button);

/* The SDK already exports app run/exit, root registration and create_page_ext. */
RTM_EXPORT(gui_app_goback);
RTM_EXPORT(gui_app_goback_to_page);
RTM_EXPORT(gui_app_this_page_memory);
RTM_EXPORT(gui_app_this_page_userdata);
RTM_EXPORT(gui_app_create_page_for_app_ext);
RTM_EXPORT(gui_app_close_anim);
RTM_EXPORT(lv_screen_active);
RTM_EXPORT(lv_color_white);
RTM_EXPORT(lv_color_black);
RTM_EXPORT(lv_obj_create);
RTM_EXPORT(lv_image_create);
RTM_EXPORT(lv_image_set_src);
RTM_EXPORT(lv_image_cache_drop);
RTM_EXPORT(lv_image_header_cache_drop);
RTM_EXPORT(lv_label_create);
RTM_EXPORT(lv_label_set_text);
RTM_EXPORT(lv_label_set_text_fmt);
RTM_EXPORT(lv_obj_set_pos);
RTM_EXPORT(lv_obj_set_size);
RTM_EXPORT(lv_obj_add_event_cb);
RTM_EXPORT(lv_obj_remove_style_all);
RTM_EXPORT(lv_obj_add_flag);
RTM_EXPORT(lv_obj_remove_flag);
RTM_EXPORT(lv_obj_set_style_text_font);
RTM_EXPORT(lv_obj_set_style_text_color);
RTM_EXPORT(lv_obj_set_style_bg_color);
RTM_EXPORT(lv_obj_set_style_bg_opa);
RTM_EXPORT(lv_obj_set_style_border_width);
RTM_EXPORT(lv_obj_set_style_border_color);
RTM_EXPORT(lv_obj_align);
RTM_EXPORT(lv_obj_center);
RTM_EXPORT(lv_event_get_user_data);
RTM_EXPORT(lv_obj_delete);
RTM_EXPORT(lv_timer_create);
RTM_EXPORT(lv_timer_delete);
RTM_EXPORT(lv_timer_pause);
RTM_EXPORT(lv_timer_resume);
RTM_EXPORT(lv_timer_get_user_data);
RTM_EXPORT(lv_canvas_get_draw_buf);
RTM_EXPORT(lv_color_hex);
RTM_EXPORT(lv_event_get_target_obj);
RTM_EXPORT(lv_group_add_obj);
RTM_EXPORT(lv_group_create);
RTM_EXPORT(lv_group_delete);
RTM_EXPORT(lv_group_focus_obj);
RTM_EXPORT(lv_group_get_focused);
RTM_EXPORT(lv_group_remove_all_objs);
RTM_EXPORT(lv_obj_get_screen);
RTM_EXPORT(lv_obj_set_flex_align);
RTM_EXPORT(lv_obj_set_flex_flow);
RTM_EXPORT(lv_obj_set_style_pad_bottom);
RTM_EXPORT(lv_obj_set_style_pad_left);
RTM_EXPORT(lv_obj_set_style_pad_right);
RTM_EXPORT(lv_obj_set_style_pad_row);
RTM_EXPORT(lv_obj_set_style_pad_top);
RTM_EXPORT(lv_obj_set_style_radius);
RTM_EXPORT(lv_obj_set_style_text_align);
RTM_EXPORT(lv_qrcode_create);
RTM_EXPORT(lv_qrcode_set_dark_color);
RTM_EXPORT(lv_qrcode_set_light_color);
RTM_EXPORT(lv_qrcode_set_quiet_zone);
RTM_EXPORT(lv_qrcode_set_size);
RTM_EXPORT(lv_qrcode_update);
RTM_EXPORT(lv_refr_now);
RTM_EXPORT(lv_tick_elaps);
RTM_EXPORT(lv_tick_get);

void epd_app_status(epd_app_status_t *out)
{
    out->battery_percent = ui_battery_percent(status.battery_percent);
    rt_strncpy(out->clock, status.clock_text[0] ? status.clock_text : "--:--", sizeof(out->clock) - 1);
    out->clock[sizeof(out->clock) - 1] = 0;
}
void epd_app_set_recent_reading(const char *title, int percent)
{
    ui_app_set_recent_reading(title, percent);
}
void epd_app_request_full_refresh(void) { epd_wave_request_full(); }
bool epd_app_open_text_settings(void) { return ui_navigation_open(UI_PAGE_TEXT_SETTINGS); }
lv_font_t *epd_app_font_create(unsigned family, unsigned size, unsigned weight,
                               uint32_t *identity, lv_font_t **fallback)
{
    return ui_font_reader_create(family, size, weight, identity, fallback);
}
void epd_app_font_destroy(lv_font_t *font) { ui_font_reader_destroy(font); }
bool epd_app_font_prepare(const lv_font_t *font, uint32_t letter, bool adjacent)
{ return ui_reader_font_prepare(font, letter, adjacent); }
RTM_EXPORT(epd_app_font_prepare);
RTM_EXPORT(lv_draw_wait_for_finish);
RTM_EXPORT(rt_work_init);
RTM_EXPORT(rt_workqueue_sysq);
RTM_EXPORT(rt_workqueue_submit_work);
RTM_EXPORT(rt_workqueue_cancel_work);
RTM_EXPORT(rt_workqueue_cancel_work_sync);
RTM_EXPORT(epd_app_status);
RTM_EXPORT(epd_app_set_recent_reading);
RTM_EXPORT(epd_app_request_full_refresh);
RTM_EXPORT(epd_app_open_text_settings);
RTM_EXPORT(epd_app_font_create);
RTM_EXPORT(epd_app_font_destroy);
RTM_EXPORT(ui_settings_enabled);
RTM_EXPORT(ui_settings_cycle);
RTM_EXPORT(ui_settings_value);
RTM_EXPORT(ui_settings_index);
RTM_EXPORT(ui_icon_bookshelf);
RTM_EXPORT(gui_app_exec_now);
RTM_EXPORT(gui_app_is_page_present);
RTM_EXPORT(lv_color_eq);
RTM_EXPORT(lv_event_get_indev);
RTM_EXPORT(lv_indev_get_point);
RTM_EXPORT(lv_label_set_long_mode);
RTM_EXPORT(lv_log_add);
RTM_EXPORT(lv_malloc);
RTM_EXPORT(lv_malloc_zeroed);
RTM_EXPORT(lv_realloc);
RTM_EXPORT(lv_free);
RTM_EXPORT(lv_obj_add_state);
RTM_EXPORT(lv_obj_remove_state);
RTM_EXPORT(lv_obj_get_child);
RTM_EXPORT(lv_obj_get_style_prop);
RTM_EXPORT(lv_obj_get_width);
RTM_EXPORT(lv_obj_has_flag);
RTM_EXPORT(lv_obj_has_state);
RTM_EXPORT(lv_obj_set_height);
RTM_EXPORT(lv_obj_set_width);
RTM_EXPORT(lv_obj_set_x);
RTM_EXPORT(lv_obj_set_y);
RTM_EXPORT(lv_obj_set_style_text_letter_space);
RTM_EXPORT(lv_obj_set_style_text_line_space);
RTM_EXPORT(lv_text_attributes_init);
RTM_EXPORT(lv_text_get_next_line);
RTM_EXPORT(lv_font_get_glyph_dsc);
RTM_EXPORT(lv_font_get_glyph_bitmap);
RTM_EXPORT(lv_font_glyph_release_draw_data);
RTM_EXPORT(lv_obj_set_style_text_decor);
RTM_EXPORT(app_service_start);
RTM_EXPORT(app_service_stop);
RTM_EXPORT(lv_event_get_code);
RTM_EXPORT(lv_keyboard_create);
RTM_EXPORT(lv_keyboard_set_mode);
RTM_EXPORT(lv_keyboard_set_textarea);
RTM_EXPORT(lv_textarea_create);
RTM_EXPORT(lv_textarea_get_text);
RTM_EXPORT(lv_textarea_set_accepted_chars);
RTM_EXPORT(lv_textarea_set_max_length);
RTM_EXPORT(lv_textarea_set_one_line);
RTM_EXPORT(lv_text_get_size);
RTM_EXPORT(lv_obj_set_style_anim_duration);
RTM_EXPORT(lv_obj_set_style_shadow_width);
