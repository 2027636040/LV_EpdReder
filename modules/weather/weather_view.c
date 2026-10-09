#include "weather_ui.h"

static uint32_t weather_poll_tick;

static const char *const weather_symbols[] = {
    "humidity", "wind", "visibility", "cloud", "sunrise", "sunset", "pressure", "air", "location"
};

static void weather_symbol_set(lv_obj_t *image, unsigned index)
{
    char path[96], name[48];
    snprintf(name, sizeof(name), "res/ui_icon_%s.ezip", weather_symbols[index]);
    bool found = storage_app_path(path, sizeof(path), "weather", STORAGE_APP_CODE, name);
    epd_app_image_set(image, found ? path : NULL, NULL);
}

void weather_pause(void)
{
    epd_app_image_release(current->weather_widgets.icon);
    for (unsigned i = 0; i < UI_WEATHER_FORECAST_COUNT; ++i)
        epd_app_image_release(current->weather_widgets.forecast_icon[i]);
    for (unsigned i = 0; i < UI_WEATHER_METRIC_COUNT + 1; ++i)
        epd_app_image_release(current->weather_widgets.symbols[i]);
}

void weather_resume(void)
{
    const ui_weather_view_t *data = ui_weather_view();
    ui_weather_icon_set(current->weather_widgets.icon, data->code, true);
    for (unsigned i = 0; i < UI_WEATHER_FORECAST_COUNT; ++i)
        ui_weather_icon_set(current->weather_widgets.forecast_icon[i], data->forecast[i].code, false);
    for (unsigned i = 0; i < UI_WEATHER_METRIC_COUNT + 1; ++i)
        weather_symbol_set(current->weather_widgets.symbols[i], i);
    weather_time_update();
}

static lv_obj_t *weather_image_create(lv_obj_t *parent, int x, int y, int size)
{
    lv_obj_t *image = icon_create(parent, NULL, x, y);
    /* Keep layout unchanged even when resources have not been installed yet. */
    lv_obj_set_size(image, size, size);
    return image;
}

void weather_data_update(void) { ui_weather_process(); }

void weather_time_update(void)
{
    const ui_weather_view_t *data = ui_weather_view();
    if (!current->weather_widgets.city || current->weather_widgets.revision == data->revision) return;
    text_update(current->weather_widgets.city, data->city);
    text_update(current->weather_widgets.time, data->update_time);
    text_update(current->weather_widgets.temperature, data->temperature);
    text_update(current->weather_widgets.description, data->description);
    text_update(current->weather_widgets.range, data->range_wind);
    text_update(current->weather_widgets.source, data->source);
    if (current->weather_widgets.code != data->code)
        ui_weather_icon_set(current->weather_widgets.icon, data->code, true);
    current->weather_widgets.code = data->code;
    for (unsigned i = 0; i < UI_WEATHER_METRIC_COUNT; ++i)
        text_update(current->weather_widgets.metrics[i], data->metrics[i]);
    for (unsigned i = 0; i < UI_WEATHER_FORECAST_COUNT; ++i)
    {
        const ui_weather_forecast_t *f = &data->forecast[i];
        text_update(current->weather_widgets.date[i], f->date);
        text_update(current->weather_widgets.text[i], f->text);
        text_update(current->weather_widgets.temperatures[i], f->temperature_range);
        text_update(current->weather_widgets.wind[i], f->wind);
        if (current->weather_widgets.forecast_code[i] != f->code)
            ui_weather_icon_set(current->weather_widgets.forecast_icon[i], f->code, false);
        current->weather_widgets.forecast_code[i] = f->code;
    }
    current->weather_widgets.revision = data->revision;
}

void weather_create(lv_obj_t *screen)
{
    weather_data_update();
    const ui_weather_view_t *data = ui_weather_view();
    current->weather_widgets.symbols[UI_WEATHER_METRIC_COUNT] =
        weather_image_create(screen, PAGE_TITLE_X, 106, 32);
    current->weather_widgets.city = label_create(screen, data->city, PAGE_TITLE_X + 44, 101, 272, ui_font_title());
    current->weather_widgets.time = label_create(screen, data->update_time, 432, 110, 220, ui_font_small());
    lv_obj_set_style_text_align(current->weather_widgets.time, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *content = panel_create(screen, MARGIN, PAGE_CONTENT_Y,
                                    CONTENT_WIDTH, HEIGHT - PAGE_CONTENT_Y - MARGIN - 84);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_radius(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_style_pad_row(content, 0, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    /* Share spare height across sections without scaling icons or text. */
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    current->weather_widgets.icon = weather_image_create(content, 0, 0, 160);
    current->weather_widgets.temperature = centered_label(content, data->temperature, 0, 0, CONTENT_WIDTH, ui_font_temperature());
    current->weather_widgets.description = centered_label(content, data->description, 0, 0, CONTENT_WIDTH, ui_font_body());
    current->weather_widgets.range = centered_label(content, data->range_wind, 0, 0, CONTENT_WIDTH, ui_font_small());

    static const char *const metric_names[] = {
        "湿度", "风速", "能见度", "云量", "日出", "日落", "气压", "空气"
    };
    const int metric_height = 80;
    const int metric_gap = 14;
    const int metric_rows = (UI_WEATHER_METRIC_COUNT + 1) / 2;
    lv_obj_t *metrics = panel_create(content, 0, 0, CONTENT_WIDTH,
                                    metric_rows * metric_height + (metric_rows - 1) * metric_gap);
    lv_obj_remove_flag(metrics, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_width(metrics, 0, 0);
    lv_obj_set_style_radius(metrics, 0, 0);
    lv_obj_set_style_pad_all(metrics, 0, 0);
    for (unsigned i = 0; i < UI_WEATHER_METRIC_COUNT; ++i)
    {
        lv_obj_t *card = panel_create(metrics, (i % 2) * 322,
                                      (i / 2) * (metric_height + metric_gap), 298, metric_height);
        current->weather_widgets.symbols[i] = weather_image_create(card, 16, 24, 32);
        label_create(card, metric_names[i], 64, 9, 216, ui_font_caption());
        current->weather_widgets.metrics[i] = label_create(card, data->metrics[i], 64, 39, 216, ui_font_body());
    }

    label_create(content, "未来三天预报", 0, 0, CONTENT_WIDTH, ui_font_body());
    const int forecast_height = 64;
    lv_obj_t *forecasts = panel_create(content, 0, 0, CONTENT_WIDTH,
                                      UI_WEATHER_FORECAST_COUNT * forecast_height);
    lv_obj_remove_flag(forecasts, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_width(forecasts, 0, 0);
    lv_obj_set_style_radius(forecasts, 0, 0);
    lv_obj_set_style_pad_all(forecasts, 0, 0);
    for (unsigned i = 0; i < UI_WEATHER_FORECAST_COUNT; ++i)
    {
        const ui_weather_forecast_t *forecast = &data->forecast[i];
        int y = i * forecast_height;
        int text_y = y + (forecast_height - ui_font_small()->line_height) / 2;
        current->weather_widgets.date[i] = label_create(forecasts, forecast->date, 0, text_y, 76, ui_font_small());
        current->weather_widgets.forecast_icon[i] = weather_image_create(forecasts, 80, y + 8, 48);
        current->weather_widgets.text[i] = label_create(forecasts, forecast->text, 138, text_y, 126, ui_font_small());
        current->weather_widgets.temperatures[i] = centered_label(forecasts, forecast->temperature_range, 272, text_y, 158, ui_font_small());
        lv_obj_t *wind = label_create(forecasts, forecast->wind, 442,
                                      y + (forecast_height - ui_font_caption()->line_height) / 2,
                                      178, ui_font_caption());
        lv_obj_set_style_text_align(wind, LV_TEXT_ALIGN_RIGHT, 0);
        current->weather_widgets.wind[i] = wind;
        current->weather_widgets.forecast_code[i] = forecast->code;
        lv_obj_t *line = panel_create(forecasts, 0, y + forecast_height - 1, CONTENT_WIDTH, 1);
        lv_obj_set_style_radius(line, 0, 0);
        lv_obj_set_style_border_color(line, lv_color_hex(0xBBBBBB), 0);
    }
    current->weather_widgets.source = centered_label(content, data->source, 0, 0, CONTENT_WIDTH, ui_font_caption());
    current->weather_widgets.code = data->code;
    current->weather_widgets.revision = data->revision;
    button_create(screen, MARGIN, HEIGHT - MARGIN - 70, CONTENT_WIDTH, 70, "更新", WEATHER_SYNC);
}

void weather_action_start(weather_job_t job)
{
    if (current->popup) return;
    bool city_job = job == WEATHER_JOB_CITY || job == WEATHER_JOB_HISTORY_CITY;
    if (weather_request_job(job, city_job ? current->city_digits : NULL, &current->weather_ticket) != RT_EOK)
    {
        popup_open(job == WEATHER_JOB_SYNC ? "同步失败，请重试" : "操作失败，请重试", NULL);
        return;
    }
    current->weather_job = job;
    popup_open("正在同步", NULL);
    lv_group_remove_all_objs(current->group);
    lv_obj_add_flag(current->popup_button, LV_OBJ_FLAG_HIDDEN);
    current->weather_popup_phase = 1;
    /* Flush the waiting message once; no spinner, animation or blinking cursor. */
    lv_refr_now(NULL);
}

void weather_ui_process(void)
{
    uint32_t now = lv_tick_get();
    if (lv_tick_elaps(weather_poll_tick) >= 250)
    {
        weather_poll_tick = now;
        weather_data_update();
    }
    if (!current || !current->foreground) return;
    if (!current->popup && lv_refreshing_done()) weather_time_update();
    if (!current->weather_popup_phase || !lv_refreshing_done()) return;
    if (current->weather_popup_phase == 1)
    {
        weather_result_t result;
        weather_get_result(&result);
        if (result.ticket == current->weather_ticket && result.busy) return;
        bool success = result.ticket == current->weather_ticket && result.success;
        const char *message = result.ticket == current->weather_ticket ? result.message : "操作失败，请重试";
        if (current->weather_job == WEATHER_JOB_SYNC) message = success ? "同步成功" : "同步失败，请重试";
        text_update(current->popup_text, message);
        if (success)
        {
            current->weather_popup_phase = 2;
            lv_refr_now(NULL);
        }
        else
        {
            current->weather_popup_phase = 0;
            lv_obj_remove_flag(current->popup_button, LV_OBJ_FLAG_HIDDEN);
            lv_group_add_obj(current->group, current->popup_button);
            lv_group_focus_obj(current->popup_button);
        }
    }
    else if (current->weather_popup_phase == 2)
    {
        current->weather_popup_tick = now;
        current->weather_popup_phase = 3;
    }
    else if (lv_tick_elaps(current->weather_popup_tick) >= 1000)
    {
        bool return_to_settings = current->weather_job == WEATHER_JOB_CITY ||
                                  current->weather_job == WEATHER_JOB_HISTORY_CITY;
        popup_close();
        if (return_to_settings)
        {
            settings_city_committed();
            weather_settings_return();
        }
        else if (launcher_current_page() == UI_PAGE_WEATHER_SETTINGS) weather_settings_render();
        else
        {
            weather_data_update();
            weather_time_update();
        }
    }
}
