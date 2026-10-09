#ifndef WEATHER_UI_H
#define WEATHER_UI_H
#include "platform/epd_app.h"
#include "weather.h"
#include "ui_weather_data.h"
#include "weather_icons.h"
#include <stdio.h>
#include <string.h>

#define WIDTH 684
#define HEIGHT 1216
#define MARGIN 32
#define CONTENT_WIDTH (WIDTH - 2 * MARGIN)
#define PAGE_TITLE_X (MARGIN + 72)
#define PAGE_TITLE_Y 103
#define PAGE_CONTENT_Y 174
enum { UI_PAGE_WEATHER, UI_PAGE_WEATHER_SETTINGS, UI_PAGE_CITY, UI_PAGE_CITY_INPUT,
       UI_PAGE_CITY_HISTORY };
enum { NAV_BACK = -2, CITY_CONFIRM = -4, POPUP_CLOSE = -5, WEATHER_SYNC = -9,
       WEATHER_IMPORT = -10, CITY_ERASE = -11, CITY_DIGIT_BASE = -40,
       HISTORY_PREVIOUS = -50, HISTORY_NEXT = -51, HISTORY_CITY_BASE = -70 };

typedef struct
{
    unsigned id;
    bool foreground;
    bool city_committed;

    lv_obj_t *screen;
    lv_timer_t *timer;
    lv_group_t *group, *page_group;
    lv_obj_t *previous_focus;
    struct
    {
        lv_obj_t *city, *time, *icon, *temperature, *description, *range, *source;
        lv_obj_t *metrics[UI_WEATHER_METRIC_COUNT];
        lv_obj_t *symbols[UI_WEATHER_METRIC_COUNT + 1];
        lv_obj_t *date[3], *forecast_icon[3], *text[3], *temperatures[3], *wind[3];
        uint32_t revision;
        int code, forecast_code[3];
    } weather_widgets;
    lv_obj_t *city_input, *weather_config_label;
    void *history_view;
    char city_digits[WEATHER_CITY_ID_MAX];
    uint32_t weather_ticket;
    weather_job_t weather_job;
    unsigned weather_popup_phase;
    uint32_t weather_popup_tick;
    lv_obj_t *popup;
    lv_obj_t *popup_text, *popup_button;
} weather_page_t;
extern weather_page_t *current;
#define ui_font_body() epd_app_font_role(EPD_FONT_BODY)
#define ui_font_small() epd_app_font_role(EPD_FONT_SMALL)
#define ui_font_title() epd_app_font_role(EPD_FONT_TITLE)
#define ui_font_caption() epd_app_font_role(EPD_FONT_CAPTION)
#define ui_font_temperature() epd_app_font_role(EPD_FONT_TEMPERATURE)
#define label_create epd_app_label
#define centered_label epd_app_centered_label
#define panel_create epd_app_panel
#define icon_create epd_app_icon
#define text_update epd_app_text
#define epd_obj_init epd_app_object_init
#define lv_refreshing_done epd_app_refresh_done
#define launcher_current_page() (current->id)

lv_obj_t *button_create(lv_obj_t *parent, int x, int y, int w, int h, const char *text, int action);
lv_obj_t *page_title_create(lv_obj_t *screen, const char *text, const lv_font_t *font);
void popup_open(const char *text, const char *detail);
void popup_close(void);
void weather_create(lv_obj_t *screen);
void weather_resume(void);
void weather_pause(void);
void weather_time_update(void);
void weather_action_start(weather_job_t job);
void weather_ui_process(void);
void weather_data_update(void);
void city_create(lv_obj_t *screen);
void city_input_create(lv_obj_t *screen);
void city_input_pause(void);
void settings_city_committed(void);
void weather_settings_render(void);
void weather_settings_create(lv_obj_t *screen);
void weather_page_open(unsigned page);
void weather_settings_return(void);
bool city_action(int target);
void city_history_create(lv_obj_t *screen);
void city_history_resume(void);
void city_history_stop(void);
bool city_history_action(int target);
#endif
