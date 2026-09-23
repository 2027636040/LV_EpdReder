#include "launcher.h"
#include "ui_font.h"
#include "icons/ui_icons.h"
#include "icons/weather_icons.h"
#include "ui_weather_data.h"
#include "weather.h"
#include "bf0_hal.h"
#include "ui_bookshelf_data.h"
#include "ui_settings.h"
#include "ui_reader.h"
#include "ui_app.h"
#include "boards/epd_e0470a03_57x/epd_waveform.h"
#include <stdio.h>
#include <string.h>

#define WIDTH 684
#define HEIGHT 1216
#define MARGIN 32
#define CONTENT_WIDTH (WIDTH - 2 * MARGIN)
#define PAGE_TITLE_X (MARGIN + 72)
#define PAGE_TITLE_Y 103
#define PAGE_CONTENT_Y 174
#define ICON_BUTTON_SIZE 56
#define BACK_BUTTON_Y 94
#define FOCUS_CAPACITY 24
#define NAV_BACK (-2)
#define NAV_IDLE (-1)
#define CITY_CONFIRM (-4)
#define POPUP_CLOSE (-5)
#define WEATHER_SYNC (-9)
#define WEATHER_IMPORT (-10)
#define CITY_ERASE (-11)
#define CITY_DIGIT_BASE (-40)
#define BOOKSHELF_PREVIOUS (-6)
#define BOOKSHELF_NEXT (-7)
#define BOOK_OPEN_BASE (-128)
#define BOOKSHELF_PAGE_SIZE 4
#define SETTINGS_SAVE (-8)
#define SETTING_CHANGE_BASE (-192)
#define READER_PREVIOUS (-210)
#define READER_NEXT (-211)
#define READER_MENU (-212)
#define READER_CONFIRM (-213)
#define READER_OPTION_PREVIOUS (-214)
#define READER_OPTION_NEXT (-215)
#define READER_OPTION_CYCLE (-216)
#define READER_MINUS5 (-217)
#define READER_MINUS1 (-218)
#define READER_PLUS1 (-219)
#define READER_PLUS5 (-220)

extern bool lv_refreshing_done(void);

typedef struct
{
    lv_obj_t *time, *wifi, *bluetooth, *battery, *percent;
    lv_obj_t *recent, *weather, *power;
} status_view_t;

typedef struct
{
    lv_obj_t *value;
    lv_obj_t *track;
    lv_obj_t *knob;
} setting_view_t;

static ui_nav_t navigation;
static lv_group_t *group;
static lv_obj_t *focus_items[FOCUS_CAPACITY];
static unsigned focus_count;
static int pending_page = NAV_IDLE;
static status_view_t view;
static launcher_status_t status;
static bool initialized;
static lv_style_t epd_style;
static struct
{
    lv_obj_t *city, *time, *icon, *temperature, *description, *range, *source;
    lv_obj_t *metrics[UI_WEATHER_METRIC_COUNT];
    lv_obj_t *date[3], *forecast_icon[3], *text[3], *temperatures[3], *wind[3];
    uint32_t revision;
    int code, forecast_code[3];
} weather_widgets;
static lv_obj_t *city_input, *weather_config_label;
static char city_digits[WEATHER_CITY_ID_MAX];
static uint32_t weather_ticket;
static weather_job_t weather_job;
static unsigned weather_popup_phase;
static uint32_t weather_popup_tick, weather_poll_tick;
static lv_obj_t *popup;
static lv_obj_t *popup_text, *popup_button;
static lv_obj_t *popup_previous_focus;
static unsigned bookshelf_first;
static setting_view_t setting_views[UI_SETTING_COUNT];
static lv_obj_t *reader_body, *reader_footer, *reader_progress, *reader_panel;
static lv_obj_t *reader_option_label, *reader_jump_label;
static uint32_t reader_revision;
static unsigned reader_jump, reader_option;
static unsigned reader_panel_focus_start;
static bool reader_session, reader_settings_dirty, reader_return_panel;
static bool reader_confirm_pending;
static bool reader_touch = true;
static unsigned reader_timeout;
static void reader_panel_open(void);

static const char *const titles[UI_PAGE_COUNT] = {
    "SiFli EPD DEMO", "书架", "天气", "文件传输", "相册", "Wi-Fi 配网",
    "设置", "阅读", "文本设置", "天气城市ID", "关于设备", "锁屏", "天气设置", "天气城市ID"
};

static void epd_obj_init(lv_obj_t *obj)
{
    lv_obj_remove_style_all(obj);
    lv_obj_add_style(obj, &epd_style, LV_PART_MAIN);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC |
                      LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

static void text_update(lv_obj_t *label, const char *text)
{
    if (label && strcmp(lv_label_get_text(label), text)) lv_label_set_text(label, text);
}

static lv_obj_t *label_create(lv_obj_t *parent, const char *text, int x, int y,
                              int width, const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent);
    epd_obj_init(label);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0x222222), 0);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    if (width > 0)
    {
        lv_obj_set_width(label, width);
        lv_obj_set_height(label, font->line_height);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    }
    return label;
}

static lv_obj_t *icon_create(lv_obj_t *parent, const lv_image_dsc_t *src, int x, int y)
{
    lv_obj_t *icon = lv_image_create(parent);
    epd_obj_init(icon);
    lv_image_set_src(icon, src);
    lv_obj_set_style_image_recolor_opa(icon, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(icon, x, y);
    return icon;
}

static lv_obj_t *panel_create(lv_obj_t *parent, int x, int y, int width, int height)
{
    lv_obj_t *panel = lv_obj_create(parent);
    epd_obj_init(panel);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, width, height);
    lv_obj_set_style_bg_color(panel, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x999999), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    return panel;
}

unsigned launcher_focus_index(void)
{
    lv_obj_t *focused = lv_group_get_focused(group);
    for (unsigned i = 0; i < focus_count; ++i)
        if (focus_items[i] == focused) return i;
    return 0;
}

static void clicked(lv_event_t *event)
{
    if (pending_page != NAV_IDLE || weather_popup_phase) return;
    int target = (int)(intptr_t)lv_event_get_user_data(event);
    if (launcher_current_page() == UI_PAGE_READER &&
        (!lv_refreshing_done() || (reader_confirm_pending && target != NAV_BACK))) return;
    lv_obj_t *button = lv_event_get_target_obj(event);
    lv_group_focus_obj(button);
    pending_page = target;
}

static lv_obj_t *button_create(lv_obj_t *parent, int x, int y, int width, int height,
                               const char *text, int target)
{
    lv_obj_t *button = panel_create(parent, x, y, width, height);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_color(button, lv_color_black(), LV_STATE_FOCUSED);
    /* Use an inset outline so focus never changes the content coordinates. */
    lv_obj_set_style_outline_color(button, lv_color_black(), LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(button, 3, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(button, -4, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(button, clicked, LV_EVENT_CLICKED, (void *)(intptr_t)target);
    lv_group_add_obj(group, button);
    LV_ASSERT(focus_count < FOCUS_CAPACITY);
    focus_items[focus_count++] = button;
    if (text)
    {
        lv_obj_t *label = label_create(button, text, 0, 0, width - 24, ui_font_body());
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(label);
    }
    return button;
}

static lv_obj_t *icon_button_create(lv_obj_t *parent, int x, int y,
                                    const lv_image_dsc_t *src, int target)
{
    lv_obj_t *button = button_create(parent, x, y, ICON_BUTTON_SIZE, ICON_BUTTON_SIZE, NULL, target);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
    lv_obj_t *icon = icon_create(button, src, 0, 0);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(icon);
    return button;
}

static lv_obj_t *page_title_create(lv_obj_t *screen, const char *text, const lv_font_t *font)
{
    return label_create(screen, text, PAGE_TITLE_X, PAGE_TITLE_Y, WIDTH - MARGIN - PAGE_TITLE_X, font);
}

static void image_update(lv_obj_t *icon, const lv_image_dsc_t *src)
{
    if (lv_image_get_src(icon) != src) lv_image_set_src(icon, src);
}

static int radio_update(lv_obj_t *icon, ui_radio_state_t state, int right,
                         const lv_image_dsc_t *on, const lv_image_dsc_t *disconnected)
{
    if (!icon) return right;
    if (state == UI_RADIO_OFF)
    {
        lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
        return right;
    }
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_HIDDEN);
    image_update(icon, state == UI_RADIO_CONNECTED ? on : disconnected);
    lv_obj_set_x(icon, right - 32);
    return right - 44;
}

static void status_render(void)
{
    char buffer[196];
    text_update(view.time, status.time_text[0] ? status.time_text : "---- -- --  --:--");
    snprintf(buffer, sizeof(buffer), "%d%%", ui_battery_percent(status.battery_percent));
    text_update(view.percent, buffer);
    static const lv_image_dsc_t *const batteries[] = {
        &ui_icon_battery_empty, &ui_icon_battery_mid,
        &ui_icon_battery_full, &ui_icon_battery_charge
    };
    if (view.battery)
        image_update(view.battery, batteries[ui_battery_icon(status.battery_percent, status.charging)]);
    int right = radio_update(view.bluetooth, status.bluetooth, WIDTH - MARGIN - (view.battery ? 116 : 0),
                              &ui_icon_bt_on, &ui_icon_bt_disconnected);
    radio_update(view.wifi, status.wifi, right, &ui_icon_wifi_on, &ui_icon_wifi_disconnected);
    if (view.recent)
    {
        if (status.recent_book[0])
            snprintf(buffer, sizeof(buffer), "最近阅读：%s (%d%%)", status.recent_book,
                     ui_battery_percent(status.reading_percent));
        else
            snprintf(buffer, sizeof(buffer), "最近阅读：暂无阅读记录");
        text_update(view.recent, buffer);
    }
    snprintf(buffer, sizeof(buffer), "天气：%s", status.weather[0] ? status.weather : "暂无天气数据");
    text_update(view.weather, buffer);
    snprintf(buffer, sizeof(buffer), "电量：%d%%%s", ui_battery_percent(status.battery_percent),
             status.charging ? " · 充电中" : "");
    text_update(view.power, buffer);
}

void launcher_set_status(const launcher_status_t *new_status)
{
    status = *new_status;
    status.time_text[sizeof(status.time_text) - 1] = '\0';
    status.recent_book[sizeof(status.recent_book) - 1] = '\0';
    status.weather[sizeof(status.weather) - 1] = '\0';
    if (initialized) status_render();
}

static void header_create(lv_obj_t *screen, bool show_battery)
{
    view.time = label_create(screen, "", MARGIN, 25, 322, ui_font_small());
    view.bluetooth = icon_create(screen, &ui_icon_bt_disconnected, 0, 22);
    view.wifi = icon_create(screen, &ui_icon_wifi_disconnected, 0, 22);
    if (show_battery)
    {
        view.battery = icon_create(screen, &ui_icon_battery_empty, WIDTH - MARGIN - 104, 18);
        view.percent = label_create(screen, "", WIDTH - MARGIN - 60, 25, 62, ui_font_small());
        lv_obj_set_style_text_align(view.percent, LV_TEXT_ALIGN_RIGHT, 0);
    }
    lv_obj_t *line = panel_create(screen, MARGIN, 74, CONTENT_WIDTH, 1);
    lv_obj_set_style_radius(line, 0, 0);
}

static void home_create(lv_obj_t *screen)
{
    static const struct
    {
        ui_page_id_t page;
        const lv_image_dsc_t *icon;
    } entries[] = {
        {UI_PAGE_BOOKSHELF, &ui_icon_bookshelf}, {UI_PAGE_WEATHER, &ui_icon_weather},
        {UI_PAGE_TRANSFER, &ui_icon_transfer}, {UI_PAGE_ALBUM, &ui_icon_album},
        {UI_PAGE_WIFI, &ui_icon_wifi_entry}, {UI_PAGE_SETTINGS, &ui_icon_settings}
    };
    label_create(screen, titles[UI_PAGE_HOME], MARGIN, 103, CONTENT_WIDTH, ui_font_title());
    for (unsigned i = 0; i < 6; ++i)
    {
        lv_obj_t *tile = button_create(screen, MARGIN + (i % 2) * 322,
                174 + (i / 2) * 202, 298, 180, NULL, entries[i].page);
        icon_create(tile, entries[i].icon, 108, 24);
        lv_obj_t *label = label_create(tile, titles[entries[i].page], 12, 126, 272, ui_font_body());
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_obj_t *quick = panel_create(screen, MARGIN, 806, CONTENT_WIDTH, 224);
    icon_create(quick, &ui_icon_quick, 22, 20);
    label_create(quick, "快捷信息", 66, 22, 400, ui_font_body());
    view.recent = label_create(quick, "", 22, 80, CONTENT_WIDTH - 46, ui_font_small());
    view.weather = label_create(quick, "", 22, 124, CONTENT_WIDTH - 46, ui_font_small());
    view.power = label_create(quick, "", 22, 166, CONTENT_WIDTH - 46, ui_font_small());
    button_create(screen, MARGIN, 1060, CONTENT_WIDTH, 70, "进入锁屏", UI_PAGE_LOCK);
}

static const lv_image_dsc_t *page_icon(ui_page_id_t page)
{
    switch (page)
    {
    case UI_PAGE_WEATHER: case UI_PAGE_CITY: return &ui_icon_weather;
    case UI_PAGE_TRANSFER: return &ui_icon_transfer;
    case UI_PAGE_ALBUM: return &ui_icon_album;
    case UI_PAGE_WIFI: return &ui_icon_wifi_entry;
    case UI_PAGE_SETTINGS: case UI_PAGE_TEXT_SETTINGS: case UI_PAGE_ABOUT: return &ui_icon_settings;
    default: return &ui_icon_bookshelf;
    }
}

static lv_obj_t *centered_label(lv_obj_t *parent, const char *text, int x, int y,
                                int width, const lv_font_t *font)
{
    lv_obj_t *label = label_create(parent, text, x, y, width, font);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    return label;
}

static void book_progress_create(lv_obj_t *parent, int x, int y, int width, unsigned percent)
{
    lv_obj_t *track = panel_create(parent, x, y, width, 8);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_width(track, 0, 0);
    lv_obj_set_style_bg_color(track, lv_color_hex(0xDDDDDD), 0);
    lv_obj_set_style_radius(track, 4, 0);
    if (percent > 100) percent = 100;
    int fill_width = (width * percent + 50) / 100;
    if (fill_width > 0)
    {
        lv_obj_t *fill = panel_create(track, 0, 0, fill_width, 8);
        lv_obj_remove_flag(fill, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_border_width(fill, 0, 0);
        lv_obj_set_style_bg_color(fill, lv_color_hex(0x222222), 0);
        lv_obj_set_style_radius(fill, 4, 0);
    }
}

static void bookshelf_page_button(lv_obj_t *screen, int x, const char *text, int action, bool enabled)
{
    if (enabled)
    {
        button_create(screen, x, HEIGHT - MARGIN - 70, 298, 70, text, action);
        return;
    }
    /* Disabled page controls remain visible but do not enter the focus group. */
    lv_obj_t *button = panel_create(screen, x, HEIGHT - MARGIN - 70, 298, 70);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_state(button, LV_STATE_DISABLED);
    lv_obj_set_style_border_color(button, lv_color_hex(0xBBBBBB), 0);
    lv_obj_t *label = centered_label(button, text, 0, 0, 274, ui_font_body());
    lv_obj_set_style_text_color(label, lv_color_hex(0x999999), 0);
    lv_obj_center(label);
}

static void bookshelf_create(lv_obj_t *screen)
{
    unsigned count = ui_bookshelf_count();
    char text[128];
    if (bookshelf_first >= count)
        bookshelf_first = count ? ((count - 1) / BOOKSHELF_PAGE_SIZE) * BOOKSHELF_PAGE_SIZE : 0;
    page_title_create(screen, "书架", ui_font_title());

    static const uint32_t cover_colors[] = {0xEEEEEE, 0xDDDDDD, 0xFFFFFF, 0xCCCCCC};
    for (unsigned slot = 0; slot < BOOKSHELF_PAGE_SIZE && bookshelf_first + slot < count; ++slot)
    {
        unsigned index = bookshelf_first + slot;
        const ui_bookshelf_book_t *book = ui_bookshelf_book(index);
        lv_obj_t *card = button_create(screen, MARGIN, 178 + slot * 210,
                                       CONTENT_WIDTH, 190, NULL, BOOK_OPEN_BASE + (int)slot);
        lv_obj_t *cover = panel_create(card, 20, 22, 112, 146);
        lv_obj_remove_flag(cover, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(cover, 4, 0);
        lv_obj_set_style_bg_color(cover, lv_color_hex(cover_colors[index % 4]), 0);
        icon_create(cover, &ui_icon_bookshelf, 15, 32);
        label_create(card, book->file.name, 152, 22, 442, ui_font_body());
        snprintf(text, sizeof(text), "%luKB | 最近阅读：%s",
                 (unsigned long)((book->file.size + 1023u) / 1024u), book->last_read);
        label_create(card, text, 152, 68, 442, ui_font_small());
        book_progress_create(card, 152, 115, 442, book->position.progress);
        if (book->position.current_page && book->position.total_pages)
            snprintf(text, sizeof(text), "进度 %u%% | 第%u/%u页",
                     (unsigned)book->position.progress, (unsigned)book->position.current_page,
                     (unsigned)book->position.total_pages);
        else if (book->position.current_page)
            snprintf(text, sizeof(text), "进度 %u%% | 第%u页",
                     (unsigned)book->position.progress, (unsigned)book->position.current_page);
        else
            snprintf(text, sizeof(text), "未读");
        label_create(card, text, 152, 141, 442, ui_font_caption());
    }
    if (count == 0)
    {
        centered_label(screen, "这里空空如也~", MARGIN,
                       (PAGE_CONTENT_Y + HEIGHT - MARGIN - ui_font_body()->line_height) / 2,
                       CONTENT_WIDTH, ui_font_body());
        return;
    }
    bookshelf_page_button(screen, MARGIN, "上一页", BOOKSHELF_PREVIOUS, bookshelf_first > 0);
    bookshelf_page_button(screen, 354, "下一页", BOOKSHELF_NEXT,
                          bookshelf_first + BOOKSHELF_PAGE_SIZE < count);
}

static void weather_time_update(void)
{
    const ui_weather_view_t *data = ui_weather_view();
    if (!weather_widgets.city || weather_widgets.revision == data->revision) return;
    text_update(weather_widgets.city, data->city);
    text_update(weather_widgets.time, data->update_time);
    text_update(weather_widgets.temperature, data->temperature);
    text_update(weather_widgets.description, data->description);
    text_update(weather_widgets.range, data->range_wind);
    text_update(weather_widgets.source, data->source);
    if (weather_widgets.code != data->code)
        lv_image_set_src(weather_widgets.icon, ui_weather_icon(data->code, true));
    weather_widgets.code = data->code;
    for (unsigned i = 0; i < UI_WEATHER_METRIC_COUNT; ++i)
        text_update(weather_widgets.metrics[i], data->metrics[i]);
    for (unsigned i = 0; i < UI_WEATHER_FORECAST_COUNT; ++i)
    {
        const ui_weather_forecast_t *f = &data->forecast[i];
        text_update(weather_widgets.date[i], f->date);
        text_update(weather_widgets.text[i], f->text);
        text_update(weather_widgets.temperatures[i], f->temperature_range);
        text_update(weather_widgets.wind[i], f->wind);
        if (weather_widgets.forecast_code[i] != f->code)
            lv_image_set_src(weather_widgets.forecast_icon[i], ui_weather_icon(f->code, false));
        weather_widgets.forecast_code[i] = f->code;
    }
    weather_widgets.revision = data->revision;
}

static void weather_create(lv_obj_t *screen)
{
    ui_weather_process();
    const ui_weather_view_t *data = ui_weather_view();
    icon_create(screen, &ui_icon_location, PAGE_TITLE_X, 106);
    weather_widgets.city = label_create(screen, data->city, PAGE_TITLE_X + 44, 101, 272, ui_font_title());
    weather_widgets.time = label_create(screen, data->update_time, 432, 110, 220, ui_font_small());
    lv_obj_set_style_text_align(weather_widgets.time, LV_TEXT_ALIGN_RIGHT, 0);

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
    weather_widgets.icon = icon_create(content, ui_weather_icon(data->code, true), 0, 0);
    weather_widgets.temperature = centered_label(content, data->temperature, 0, 0, CONTENT_WIDTH, ui_font_temperature());
    weather_widgets.description = centered_label(content, data->description, 0, 0, CONTENT_WIDTH, ui_font_body());
    weather_widgets.range = centered_label(content, data->range_wind, 0, 0, CONTENT_WIDTH, ui_font_small());

    static const char *const metric_names[] = {
        "湿度", "风速", "能见度", "云量", "日出", "日落", "气压", "空气"
    };
    static const lv_image_dsc_t *const metric_icons[] = {
        &ui_icon_humidity, &ui_icon_wind, &ui_icon_visibility, &ui_icon_cloud,
        &ui_icon_sunrise, &ui_icon_sunset, &ui_icon_pressure, &ui_icon_air
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
        icon_create(card, metric_icons[i], 16, 24);
        label_create(card, metric_names[i], 64, 9, 216, ui_font_caption());
        weather_widgets.metrics[i] = label_create(card, data->metrics[i], 64, 39, 216, ui_font_body());
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
        weather_widgets.date[i] = label_create(forecasts, forecast->date, 0, text_y, 76, ui_font_small());
        weather_widgets.forecast_icon[i] = icon_create(forecasts, ui_weather_icon(forecast->code, false), 80, y + 8);
        weather_widgets.text[i] = label_create(forecasts, forecast->text, 138, text_y, 126, ui_font_small());
        weather_widgets.temperatures[i] = centered_label(forecasts, forecast->temperature_range, 272, text_y, 158, ui_font_small());
        lv_obj_t *wind = label_create(forecasts, forecast->wind, 442,
                                      y + (forecast_height - ui_font_caption()->line_height) / 2,
                                      178, ui_font_caption());
        lv_obj_set_style_text_align(wind, LV_TEXT_ALIGN_RIGHT, 0);
        weather_widgets.wind[i] = wind;
        weather_widgets.forecast_code[i] = forecast->code;
        lv_obj_t *line = panel_create(forecasts, 0, y + forecast_height - 1, CONTENT_WIDTH, 1);
        lv_obj_set_style_radius(line, 0, 0);
        lv_obj_set_style_border_color(line, lv_color_hex(0xBBBBBB), 0);
    }
    weather_widgets.source = centered_label(content, data->source, 0, 0, CONTENT_WIDTH, ui_font_caption());
    weather_widgets.code = data->code;
    weather_widgets.revision = data->revision;
    button_create(screen, MARGIN, HEIGHT - MARGIN - 70, CONTENT_WIDTH, 70, "更新", WEATHER_SYNC);
}

static void city_create(lv_obj_t *screen)
{
    page_title_create(screen, "天气城市ID", ui_font_title());
    lv_obj_t *quiet = panel_create(screen, (WIDTH - 348) / 2, 296, 348, 348);
    lv_obj_set_style_border_width(quiet, 0, 0);
    lv_obj_set_style_radius(quiet, 0, 0);
    lv_obj_t *qr = lv_qrcode_create(quiet);
    epd_obj_init(qr);
    lv_qrcode_set_size(qr, 300);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_set_quiet_zone(qr, true);
    lv_obj_center(qr);
    if (lv_qrcode_update(qr, WEATHER_CITY_LOOKUP_URL, strlen(WEATHER_CITY_LOOKUP_URL)) != LV_RESULT_OK)
        centered_label(screen, "二维码生成失败", MARGIN, 420, CONTENT_WIDTH, ui_font_body());
#if defined(PSRAM_CACHE_WB)
    /* The canvas includes an indexed palette; write back it and every pixel row. */
    lv_draw_buf_t *qr_buffer = lv_canvas_get_draw_buf(qr);
    if (qr_buffer) mpu_dcache_clean(qr_buffer->data, qr_buffer->data_size);
#endif
    centered_label(screen, "扫码查看城市ID", MARGIN, 696, CONTENT_WIDTH, ui_font_body());
    button_create(screen, 112, 800, 460, 80, "我已获取城市ID", UI_PAGE_CITY_INPUT);
}

static void city_input_create(lv_obj_t *screen)
{
    weather_config_t cfg;
    weather_get_config(&cfg);
    snprintf(city_digits, sizeof(city_digits), "%s", cfg.city_id);
    page_title_create(screen, "天气城市ID", ui_font_title());
    centered_label(screen, "请输入数字城市ID", MARGIN, 238, CONTENT_WIDTH, ui_font_body());
    lv_obj_t *input = panel_create(screen, 64, 302, 556, 88);
    city_input = centered_label(input, city_digits, 16,
        (88 - ui_font_title()->line_height) / 2, 524, ui_font_title());
    /* A static label avoids the textarea cursor's periodic blinking on e-paper. */
    lv_obj_t *keys = panel_create(screen, MARGIN, 724, CONTENT_WIDTH, 436);
    static const char *const labels[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "确认", "退"};
    for (unsigned i = 0; i < 12; ++i)
    {
        int target = i < 9 ? CITY_DIGIT_BASE + (int)i + 1 :
                     i == 9 ? CITY_DIGIT_BASE : i == 10 ? CITY_CONFIRM : CITY_ERASE;
        button_create(keys, 18 + (i % 3) * 198, 18 + (i / 3) * 102, 186, 88, labels[i], target);
    }
}

static void weather_settings_render(void)
{
    weather_config_t cfg;
    char text[128];
    weather_get_config(&cfg);
    snprintf(text, sizeof(text), "天气配置：%s   城市：%s", cfg.api_key[0] ? "已导入" : "未导入",
             cfg.city[0] ? cfg.city : "未设置");
    text_update(weather_config_label, text);
}

static void weather_settings_create(lv_obj_t *screen)
{
    page_title_create(screen, "天气设置", ui_font_title());
    weather_config_label = centered_label(screen, "", MARGIN, 246, CONTENT_WIDTH, ui_font_small());
    weather_settings_render();
    button_create(screen, MARGIN, 348, CONTENT_WIDTH, 84, "从 TF 卡导入天气配置", WEATHER_IMPORT);
    button_create(screen, MARGIN, 464, CONTENT_WIDTH, 84, "天气城市ID", UI_PAGE_CITY);
    centered_label(screen, "配置文件：qweather.json", MARGIN, 608, CONTENT_WIDTH, ui_font_small());
}

static void setting_render(ui_setting_id_t id)
{
    setting_view_t *setting = &setting_views[id];
    if (setting->track)
    {
        bool enabled = ui_settings_enabled(id);
        lv_obj_set_style_bg_color(setting->track,
                                  lv_color_hex(enabled ? 0x555555 : 0xBBBBBB), 0);
        lv_obj_set_x(setting->knob, enabled ? 40 : 4);
    }
    else
        text_update(setting->value, ui_settings_value(id));
}

static void setting_row_create(lv_obj_t *screen, ui_setting_id_t id, int y, int height)
{
    const ui_setting_t *setting = ui_settings_item(id);
    setting_view_t *item = &setting_views[id];
    lv_obj_t *row = button_create(screen, MARGIN, y, CONTENT_WIDTH, height,
                                  NULL, SETTING_CHANGE_BASE + id);
    int text_y = (height - ui_font_body()->line_height) / 2;
    label_create(row, setting->label, 24, text_y, 292, ui_font_body());
    if (setting->is_switch)
    {
        item->track = panel_create(row, CONTENT_WIDTH - 104, (height - 44) / 2, 80, 44);
        lv_obj_remove_flag(item->track, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_border_width(item->track, 0, 0);
        lv_obj_set_style_radius(item->track, 22, 0);
        item->knob = panel_create(item->track, 4, 4, 36, 36);
        lv_obj_remove_flag(item->knob, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_border_width(item->knob, 0, 0);
        lv_obj_set_style_radius(item->knob, 18, 0);
    }
    else
    {
        item->value = label_create(row, "", 332, text_y, 264, ui_font_body());
        lv_obj_set_style_text_align(item->value, LV_TEXT_ALIGN_RIGHT, 0);
    }
    setting_render(id);
}

static void setting_link_create(lv_obj_t *screen, const char *text, int y, ui_page_id_t page)
{
    lv_obj_t *row = button_create(screen, MARGIN, y, CONTENT_WIDTH, 80, NULL, page);
    int text_y = (80 - ui_font_body()->line_height) / 2;
    label_create(row, text, 24, text_y, CONTENT_WIDTH - 100, ui_font_body());
    lv_obj_t *arrow = label_create(row, "›", CONTENT_WIDTH - 64, text_y, 40, ui_font_body());
    lv_obj_set_style_text_align(arrow, LV_TEXT_ALIGN_RIGHT, 0);
}

static void settings_create(lv_obj_t *screen)
{
    page_title_create(screen, "通用功能设置页面", ui_font_title());
    for (unsigned i = UI_SETTING_TOUCH; i < UI_SETTING_FONT; ++i)
        setting_row_create(screen, (ui_setting_id_t)i, 174 + i * 84, 72);
    setting_link_create(screen, "进入文本设置", 762, UI_PAGE_TEXT_SETTINGS);
    setting_link_create(screen, "天气设置", 850, UI_PAGE_WEATHER_SETTINGS);
    setting_link_create(screen, "关于设备", 938, UI_PAGE_ABOUT);
    button_create(screen, MARGIN, HEIGHT - MARGIN - 70, CONTENT_WIDTH, 70, "保存设置", SETTINGS_SAVE);
}

static void text_settings_create(lv_obj_t *screen)
{
    page_title_create(screen, "文本设置", ui_font_title());
    for (unsigned i = UI_SETTING_FONT; i < UI_SETTING_COUNT; ++i)
        setting_row_create(screen, (ui_setting_id_t)i, 174 + (i - UI_SETTING_FONT) * 132, 110);
}

static void popup_close(void)
{
    if (!popup) return;
    lv_obj_delete(popup);
    popup = NULL;
    popup_text = popup_button = NULL;
    weather_popup_phase = 0;
    /* The dialog button was appended after the page's focusable controls. */
    if (focus_count) focus_items[--focus_count] = NULL;
    for (unsigned i = 0; i < focus_count; ++i) lv_group_add_obj(group, focus_items[i]);
    if (popup_previous_focus) lv_group_focus_obj(popup_previous_focus);
    popup_previous_focus = NULL;
}

static void popup_open(const char *text, const char *detail)
{
    if (popup) return;
    popup_previous_focus = lv_group_get_focused(group);
    lv_group_remove_all_objs(group);
    popup = panel_create(lv_screen_active(), 0, 0, WIDTH, HEIGHT);
    lv_obj_add_flag(popup, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(popup, 0, 0);
    lv_obj_set_style_border_width(popup, 0, 0);
    lv_obj_set_style_bg_color(popup, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(popup, LV_OPA_TRANSP, 0);
    lv_obj_t *dialog = panel_create(popup, 54, 438, 576, 282);
    lv_obj_set_style_border_color(dialog, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_width(dialog, 2, 0);
    popup_text = centered_label(dialog, text, 24, detail ? 44 : 74, 528, ui_font_body());
    if (detail) centered_label(dialog, detail, 24, 102, 528, ui_font_small());
    popup_button = button_create(dialog, 108, 184, 360, 64, "确定", POPUP_CLOSE);
    lv_group_focus_obj(popup_button);
}

static void placeholder_create(lv_obj_t *screen, ui_page_id_t page)
{
    page_title_create(screen, titles[page], ui_font_title());
    icon_create(screen, page_icon(page), (WIDTH - 80) / 2, 318);
    lv_obj_t *tip = label_create(screen, page == UI_PAGE_WIFI ? "配网功能暂未开放" : "页面入口已预留",
                                 MARGIN, 446, CONTENT_WIDTH, ui_font_body());
    lv_obj_set_style_text_align(tip, LV_TEXT_ALIGN_CENTER, 0);
}

static void reader_render(void)
{
    const ui_reader_view_t *reading = ui_reader_view();
    if (!reader_body) return;
    char text[196];
    if (reading->error[0]) text_update(reader_footer, reading->error);
    else if (!reading->ready)
    {
        text_update(reader_body, "正在排版，请稍候…");
        text_update(reader_footer, "");
    }
    else
    {
        text_update(reader_body, reading->text[0] ? reading->text : "本书暂无正文");
        if (reading->pages)
            snprintf(text, sizeof(text), "%u / %u 页 · %u%%%s", reading->page, reading->pages,
                     reading->percent, reading->saved ? "" : " · 未能保存进度");
        else
            snprintf(text, sizeof(text), "%u / -- 页 · %u%%%s", reading->page, reading->percent,
                     reading->saved ? "" : " · 未能保存进度");
        text_update(reader_footer, text);
        int width = CONTENT_WIDTH * reading->percent / 100;
        if (width) lv_obj_remove_flag(reader_progress, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(reader_progress, LV_OBJ_FLAG_HIDDEN);
        if (lv_obj_get_width(reader_progress) != width) lv_obj_set_width(reader_progress, width);
    }
    reader_revision = reading->revision;
}

static void reader_touch_event(lv_event_t *event)
{
    if (pending_page != NAV_IDLE || reader_panel || popup || !lv_refreshing_done()) return;
    if (ui_reader_waiting()) return;
    if (!reader_touch || !ui_settings_enabled(UI_SETTING_TOUCH)) return;
    lv_indev_t *indev = lv_event_get_indev(event);
    if (!indev) return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    pending_page = point.x < WIDTH / 3 ? READER_PREVIOUS :
                   point.x > WIDTH * 2 / 3 ? READER_NEXT : READER_MENU;
}

static void reader_create(lv_obj_t *screen)
{
    const ui_reader_view_t *reading = ui_reader_view();
    page_title_create(screen, reading->title, ui_font_small());
    reader_body = label_create(screen, "", reading->margin, UI_READER_TOP,
                                WIDTH - 2 * reading->margin, reading->font);
    lv_label_set_long_mode(reader_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(reader_body, UI_READER_BOTTOM - UI_READER_TOP);
    lv_obj_set_style_text_line_space(reader_body, reading->line_space, 0);
    lv_obj_set_style_text_letter_space(reader_body, 0, 0);
    lv_obj_set_style_text_color(reader_body, lv_color_black(), 0);
    lv_obj_add_flag(reader_body, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(reader_body, reader_touch_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *track = panel_create(screen, MARGIN, UI_READER_BOTTOM + 20, CONTENT_WIDTH, 8);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(track, 0, 0);
    reader_progress = panel_create(track, 0, 0, 0, 8);
    lv_obj_remove_flag(reader_progress, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(reader_progress, 0, 0);
    lv_obj_set_style_border_width(reader_progress, 0, 0);
    lv_obj_set_style_bg_color(reader_progress, lv_color_black(), 0);
    reader_footer = centered_label(screen, "", MARGIN, UI_READER_BOTTOM + 40,
                                   CONTENT_WIDTH, ui_font_caption());
    reader_render();
}

static void reader_option_render(void)
{
    char text[96];
    static const char *const timeouts[] = {"5分钟", "10分钟", "30分钟", "不关机"};
    if (reader_option == 0) snprintf(text, sizeof(text), "触摸翻页：%s", reader_touch ? "开" : "关");
    else if (reader_option == 1)
        snprintf(text, sizeof(text), "全刷周期：%s", ui_settings_value(UI_SETTING_FULL_REFRESH));
    else snprintf(text, sizeof(text), "超时关机：%s", timeouts[reader_timeout]);
    text_update(reader_option_label, text);
    snprintf(text, sizeof(text), "%u", reader_jump);
    text_update(reader_jump_label, text);
}

static void reader_panel_close(void)
{
    if (!reader_panel) return;
    lv_group_remove_all_objs(group);
    lv_obj_delete(reader_panel);
    reader_panel = reader_option_label = reader_jump_label = NULL;
    reader_confirm_pending = false;
    while (focus_count > reader_panel_focus_start) focus_items[--focus_count] = NULL;
    for (unsigned i = 0; i < focus_count; ++i) lv_group_add_obj(group, focus_items[i]);
    if (focus_count) lv_group_focus_obj(focus_items[0]);
}

static void reader_panel_open(void)
{
    if (reader_panel || !ui_reader_view()->ready) return;
    reader_panel_focus_start = focus_count;
    reader_jump = ui_reader_view()->page;
    reader_panel = panel_create(lv_screen_active(), MARGIN, 798, CONTENT_WIDTH, 390);
    lv_obj_set_style_border_color(reader_panel, lv_color_black(), 0);
    lv_obj_set_style_border_width(reader_panel, 2, 0);
    centered_label(reader_panel, "阅读设置", 18, 20, 584, ui_font_body());
    button_create(reader_panel, 18, 74, 60, 66, "<", READER_OPTION_PREVIOUS);
    lv_obj_t *choice = button_create(reader_panel, 88, 74, 444, 66, NULL, READER_OPTION_CYCLE);
    reader_option_label = centered_label(choice, "", 8, 18, 424, ui_font_body());
    button_create(reader_panel, 542, 74, 60, 66, ">", READER_OPTION_NEXT);
    button_create(reader_panel, 18, 162, 94, 66, "-5", READER_MINUS5);
    button_create(reader_panel, 122, 162, 94, 66, "-1", READER_MINUS1);
    reader_jump_label = centered_label(reader_panel, "", 226, 180, 168, ui_font_body());
    button_create(reader_panel, 404, 162, 94, 66, "+1", READER_PLUS1);
    button_create(reader_panel, 508, 162, 94, 66, "+5", READER_PLUS5);
    button_create(reader_panel, 18, 268, 282, 74, "确认", READER_CONFIRM);
    button_create(reader_panel, 318, 268, 282, 74, "文本设置", UI_PAGE_TEXT_SETTINGS);
    reader_option_render();
    lv_group_focus_obj(focus_items[reader_panel_focus_start]);
}

static void lock_create(lv_obj_t *screen)
{
    lv_obj_t *label = label_create(screen, "SiFli EPD DEMO", MARGIN, 290,
                                    CONTENT_WIDTH, ui_font_body());
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    icon_create(screen, &ui_icon_bookshelf, (WIDTH - 80) / 2, 388);
    label = label_create(screen, "Welcome", MARGIN, 512, CONTENT_WIDTH, ui_font_title());
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *unlock = icon_button_create(screen, (WIDTH - ICON_BUTTON_SIZE) / 2, 646,
                                         &ui_icon_key2, NAV_BACK);
    lv_obj_set_style_outline_width(unlock, 0, LV_STATE_FOCUSED);
}

static void page_create(void)
{
    lv_group_remove_all_objs(group);
    focus_count = 0;
    reader_panel_focus_start = 0;
    memset(&view, 0, sizeof(view));
    memset(&weather_widgets, 0, sizeof(weather_widgets));
    city_input = weather_config_label = NULL;
    memset(setting_views, 0, sizeof(setting_views));
    popup = NULL;
    popup_previous_focus = NULL;
    popup_text = popup_button = NULL;
    weather_popup_phase = 0;
    reader_body = reader_footer = reader_progress = reader_panel = NULL;
    reader_option_label = reader_jump_label = NULL;
    lv_obj_t *screen = lv_obj_create(NULL);
    epd_obj_init(screen);
    lv_obj_set_size(screen, WIDTH, HEIGHT);
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(screen, ui_font_body(), 0);
    ui_nav_entry_t *entry = ui_nav_current(&navigation);
    if (entry->page != UI_PAGE_READER) header_create(screen, entry->page != UI_PAGE_LOCK);
    if (entry->page == UI_PAGE_HOME) home_create(screen);
    else if (entry->page == UI_PAGE_LOCK) lock_create(screen);
    else if (entry->page == UI_PAGE_BOOKSHELF) bookshelf_create(screen);
    else if (entry->page == UI_PAGE_READER) reader_create(screen);
    else if (entry->page == UI_PAGE_WEATHER) weather_create(screen);
    else if (entry->page == UI_PAGE_SETTINGS) settings_create(screen);
    else if (entry->page == UI_PAGE_TEXT_SETTINGS) text_settings_create(screen);
    else if (entry->page == UI_PAGE_CITY) city_create(screen);
    else if (entry->page == UI_PAGE_CITY_INPUT) city_input_create(screen);
    else if (entry->page == UI_PAGE_WEATHER_SETTINGS) weather_settings_create(screen);
    else placeholder_create(screen, entry->page);
    if (entry->page != UI_PAGE_HOME && entry->page != UI_PAGE_LOCK)
        icon_button_create(screen, MARGIN, BACK_BUTTON_Y, &ui_icon_back, NAV_BACK);
    if (focus_count) lv_group_focus_obj(focus_items[entry->focus < focus_count ? entry->focus : 0]);
    status_render();
    /* A zero-duration load deletes the previous tree without an animation timer. */
    lv_screen_load_anim(screen, LV_SCREEN_LOAD_ANIM_NONE, 0, 0, true);
}

bool launcher_init(void)
{
    if (initialized) return true;
    if (!ui_font_init()) return false;
    ui_bookshelf_refresh();
    const ui_bookshelf_book_t *recent = NULL;
    for (unsigned i = 0; i < ui_bookshelf_count(); ++i)
    {
        const ui_bookshelf_book_t *book = ui_bookshelf_book(i);
        if (book->position.current_page && (!recent || (int32_t)(book->order - recent->order) > 0))
            recent = book;
    }
    if (recent) ui_app_set_recent_reading(recent->file.name, recent->position.progress);
    epd_wave_set_part_times(ui_settings_refresh_count());
    group = lv_group_create();
    if (!group) return false;
    /* Use static, flat styles without the default theme's interaction effects. */
    lv_display_set_theme(NULL, NULL);
    lv_style_init(&epd_style);
    lv_style_set_transition(&epd_style, NULL);
    lv_style_set_anim_duration(&epd_style, 0);
    lv_style_set_bg_grad_dir(&epd_style, LV_GRAD_DIR_NONE);
    lv_style_set_bg_grad(&epd_style, NULL);
    lv_style_set_shadow_width(&epd_style, 0);
    lv_group_set_wrap(group, true);
    ui_nav_init(&navigation);
    ui_weather_process();
    ui_app_set_weather_summary(ui_weather_view()->summary);
    page_create();
    initialized = true;
    return true;
}

ui_page_id_t launcher_current_page(void)
{
    return ui_nav_current(&navigation)->page;
}

void launcher_open(ui_page_id_t page)
{
    if (weather_popup_phase) return;
    if (page == UI_PAGE_READER && !reader_session) return;
    if (initialized && pending_page == NAV_IDLE && page >= UI_PAGE_HOME && page < UI_PAGE_COUNT)
        pending_page = page;
}

void launcher_key(launcher_key_t key)
{
    if (!initialized || pending_page != NAV_IDLE || weather_popup_phase) return;
    if (launcher_current_page() == UI_PAGE_READER)
    {
        if (!lv_refreshing_done()) return;
        if (reader_confirm_pending)
        {
            if (key == LAUNCHER_KEY_BACK || key == LAUNCHER_KEY_ENTER) pending_page = NAV_BACK;
            return;
        }
        if (!reader_panel && !popup)
        {
            if (ui_reader_waiting() && key != LAUNCHER_KEY_ENTER) return;
            switch (key)
            {
            case LAUNCHER_KEY_PREVIOUS: pending_page = READER_PREVIOUS; break;
            case LAUNCHER_KEY_NEXT: pending_page = READER_NEXT; break;
            case LAUNCHER_KEY_ENTER: pending_page = NAV_BACK; break;
            case LAUNCHER_KEY_BACK: pending_page = READER_MENU; break;
            }
            return;
        }
        if (reader_panel && key == LAUNCHER_KEY_BACK)
        {
            pending_page = READER_CONFIRM;
            return;
        }
    }
    if (launcher_current_page() == UI_PAGE_LOCK)
    {
        if (key == LAUNCHER_KEY_ENTER) pending_page = NAV_BACK;
        return;
    }
    switch (key)
    {
    case LAUNCHER_KEY_PREVIOUS: lv_group_focus_prev(group); break;
    case LAUNCHER_KEY_NEXT: lv_group_focus_next(group); break;
    case LAUNCHER_KEY_ENTER:
    {
        lv_obj_t *focused = lv_group_get_focused(group);
        if (focused) lv_obj_send_event(focused, LV_EVENT_CLICKED, NULL);
        break;
    }
    case LAUNCHER_KEY_BACK:
        if (popup) pending_page = POPUP_CLOSE;
        else if (navigation.depth > 1) pending_page = NAV_BACK;
        break;
    }
}

static void weather_action_start(weather_job_t job)
{
    if (popup) return;
    if (weather_request_job(job, job == WEATHER_JOB_CITY ? city_digits : NULL, &weather_ticket) != RT_EOK)
    {
        popup_open(job == WEATHER_JOB_SYNC ? "同步失败，请重试" : "操作失败，请重试", NULL);
        return;
    }
    weather_job = job;
    popup_open("正在同步", NULL);
    lv_group_remove_all_objs(group);
    lv_obj_add_flag(popup_button, LV_OBJ_FLAG_HIDDEN);
    weather_popup_phase = 1;
    /* Flush the waiting message once; no spinner, animation or blinking cursor. */
    lv_refr_now(NULL);
}

static void weather_ui_process(void)
{
    uint32_t now = lv_tick_get();
    if (lv_tick_elaps(weather_poll_tick) >= 250)
    {
        weather_poll_tick = now;
        if (ui_weather_process()) ui_app_set_weather_summary(ui_weather_view()->summary);
    }
    if (!popup && lv_refreshing_done()) weather_time_update();
    if (!weather_popup_phase || !lv_refreshing_done()) return;
    if (weather_popup_phase == 1)
    {
        weather_result_t result;
        weather_get_result(&result);
        if (result.ticket == weather_ticket && result.busy) return;
        bool success = result.ticket == weather_ticket && result.success;
        const char *message = result.ticket == weather_ticket ? result.message : "操作失败，请重试";
        if (weather_job == WEATHER_JOB_SYNC) message = success ? "同步成功" : "同步失败，请重试";
        text_update(popup_text, message);
        if (success)
        {
            weather_popup_phase = 2;
            lv_refr_now(NULL);
        }
        else
        {
            weather_popup_phase = 0;
            lv_obj_remove_flag(popup_button, LV_OBJ_FLAG_HIDDEN);
            lv_group_add_obj(group, popup_button);
            lv_group_focus_obj(popup_button);
        }
    }
    else if (weather_popup_phase == 2)
    {
        weather_popup_tick = now;
        weather_popup_phase = 3;
    }
    else if (lv_tick_elaps(weather_popup_tick) >= 1000)
    {
        bool return_to_settings = weather_job == WEATHER_JOB_CITY;
        popup_close();
        if (return_to_settings)
        {
            while (navigation.depth > 1 && launcher_current_page() != UI_PAGE_SETTINGS)
                ui_nav_back(&navigation);
            page_create();
        }
        else if (launcher_current_page() == UI_PAGE_WEATHER_SETTINGS) weather_settings_render();
        else
        {
            ui_weather_process();
            weather_time_update();
        }
    }
}

void launcher_process(void)
{
    if (!initialized) return;
    weather_ui_process();
    if (launcher_current_page() == UI_PAGE_READER && (!reader_panel || reader_confirm_pending) &&
        !popup && lv_refreshing_done())
    {
        ui_reader_process();
        if (reader_confirm_pending)
        {
            const ui_reader_view_t *reading = ui_reader_view();
            unsigned target = reading->pages && reader_jump > reading->pages ? reading->pages : reader_jump;
            if (reading->page == target || reading->error[0])
            {
                reader_panel_close();
                reader_render();
            }
        }
        if (ui_reader_view()->revision != reader_revision) reader_render();
        if (reader_return_panel && ui_reader_view()->ready)
        {
            reader_return_panel = false;
            reader_panel_open();
        }
    }
    if (pending_page == NAV_IDLE) return;
    if (launcher_current_page() == UI_PAGE_READER && !lv_refreshing_done()) return;
    int target = pending_page;
    pending_page = NAV_IDLE;
    if (launcher_current_page() == UI_PAGE_READER)
    {
        if (target == READER_PREVIOUS || target == READER_NEXT)
        {
            ui_reader_turn(target == READER_PREVIOUS ? -1 : 1);
            ui_reader_process();
            if (ui_reader_view()->revision != reader_revision) reader_render();
            return;
        }
        if (target == READER_MENU) { reader_panel_open(); return; }
        if (reader_panel)
        {
            switch (target)
            {
            case READER_OPTION_PREVIOUS: reader_option = (reader_option + 2) % 3; break;
            case READER_OPTION_NEXT: reader_option = (reader_option + 1) % 3; break;
            case READER_OPTION_CYCLE:
                if (reader_option == 0) reader_touch = !reader_touch;
                else if (reader_option == 1)
                {
                    ui_settings_cycle(UI_SETTING_FULL_REFRESH);
                    epd_wave_set_part_times(ui_settings_refresh_count());
                }
                else reader_timeout = (reader_timeout + 1) % 4;
                break;
            case READER_MINUS5: reader_jump = reader_jump > 5 ? reader_jump - 5 : 1; break;
            case READER_MINUS1: if (reader_jump > 1) --reader_jump; break;
            case READER_PLUS1: ++reader_jump; break;
            case READER_PLUS5: reader_jump += 5; break;
            case READER_CONFIRM:
                ui_reader_seek(reader_jump);
                reader_confirm_pending = true;
                return;
            default: break;
            }
            if (target <= READER_OPTION_PREVIOUS && target >= READER_PLUS5)
            {
                unsigned last = ui_reader_view()->pages;
                if (last && reader_jump > last) reader_jump = last;
                if (reader_jump > 65536) reader_jump = 65536;
                reader_option_render();
                return;
            }
        }
        if (target == UI_PAGE_TEXT_SETTINGS)
        {
            reader_return_panel = true;
            reader_settings_dirty = false;
        }
    }
    if (target == POPUP_CLOSE)
    {
        popup_close();
        return;
    }
    if (target == WEATHER_SYNC && launcher_current_page() == UI_PAGE_WEATHER)
    {
        weather_action_start(WEATHER_JOB_SYNC);
        return;
    }
    if (target == WEATHER_IMPORT && launcher_current_page() == UI_PAGE_WEATHER_SETTINGS)
    {
        weather_action_start(WEATHER_JOB_IMPORT);
        return;
    }
    if (launcher_current_page() == UI_PAGE_SETTINGS || launcher_current_page() == UI_PAGE_TEXT_SETTINGS)
    {
        if (target >= SETTING_CHANGE_BASE && target < SETTING_CHANGE_BASE + UI_SETTING_COUNT)
        {
            ui_setting_id_t id = (ui_setting_id_t)(target - SETTING_CHANGE_BASE);
            if ((id >= UI_SETTING_FONT) != (launcher_current_page() == UI_PAGE_TEXT_SETTINGS)) return;
            ui_settings_cycle(id);
            if (id == UI_SETTING_FULL_REFRESH) epd_wave_set_part_times(ui_settings_refresh_count());
            if (reader_session && id >= UI_SETTING_FONT && id != UI_SETTING_FONT_WEIGHT)
                reader_settings_dirty = true;
            setting_render(id);
            return;
        }
        if (target == SETTINGS_SAVE && launcher_current_page() == UI_PAGE_SETTINGS)
        {
            popup_open("设置保存成功！", NULL);
            return;
        }
    }
    if (launcher_current_page() == UI_PAGE_BOOKSHELF)
    {
        if (target == BOOKSHELF_PREVIOUS || target == BOOKSHELF_NEXT)
        {
            if (target == BOOKSHELF_PREVIOUS && bookshelf_first > 0)
                bookshelf_first -= BOOKSHELF_PAGE_SIZE;
            else if (target == BOOKSHELF_NEXT && bookshelf_first + BOOKSHELF_PAGE_SIZE < ui_bookshelf_count())
                bookshelf_first += BOOKSHELF_PAGE_SIZE;
            else
                return;
            ui_nav_current(&navigation)->focus = 0;
            page_create();
            return;
        }
        if (target >= BOOK_OPEN_BASE && target < BOOK_OPEN_BASE + BOOKSHELF_PAGE_SIZE)
        {
            unsigned index = bookshelf_first + (unsigned)(target - BOOK_OPEN_BASE);
            if (!ui_bookshelf_book(index)) return;
            if (!ui_reader_open(index))
            {
                popup_open("无法打开书籍", "请检查文件或存储设备");
                return;
            }
            reader_session = true;
            reader_return_panel = false;
            target = UI_PAGE_READER;
        }
    }
    if (launcher_current_page() == UI_PAGE_CITY_INPUT)
    {
        size_t length = strlen(city_digits);
        if (target >= CITY_DIGIT_BASE && target < CITY_DIGIT_BASE + 10)
        {
            if (length + 1 < sizeof(city_digits))
            {
                city_digits[length] = '0' + target - CITY_DIGIT_BASE;
                city_digits[length + 1] = '\0';
                text_update(city_input, city_digits);
            }
            return;
        }
        if (target == CITY_ERASE)
        {
            if (length) city_digits[length - 1] = '\0';
            text_update(city_input, city_digits);
            return;
        }
        if (target == CITY_CONFIRM)
        {
            if (!length) popup_open("请输入数字城市ID", NULL);
            else weather_action_start(WEATHER_JOB_CITY);
            return;
        }
    }
    ui_page_id_t old_page = launcher_current_page();
    if (reader_session && (old_page == UI_PAGE_READER || old_page == UI_PAGE_TEXT_SETTINGS))
    {
        ui_reader_save();
        const ui_reader_view_t *reading = ui_reader_view();
        snprintf(status.recent_book, sizeof(status.recent_book), "%s", reading->title);
        status.reading_percent = reading->percent;
    }
    ui_nav_current(&navigation)->focus = (uint8_t)launcher_focus_index();
    bool changed = target == NAV_BACK ? ui_nav_back(&navigation) :
                   ui_nav_push(&navigation, (ui_page_id_t)target);
    if (changed)
    {
        ui_page_id_t page = launcher_current_page();
        if (page == UI_PAGE_BOOKSHELF && !reader_session) ui_bookshelf_refresh();
        if (page == UI_PAGE_READER && old_page == UI_PAGE_TEXT_SETTINGS && reader_settings_dirty)
        {
            ui_reader_reflow();
            ui_reader_process();
            reader_settings_dirty = false;
        }
        if (page == UI_PAGE_READER || old_page == UI_PAGE_READER) epd_wave_request_full();
        page_create();
        if (page == UI_PAGE_READER && reader_return_panel && ui_reader_view()->ready)
        {
            reader_return_panel = false;
            reader_panel_open();
        }
        if (reader_session && page != UI_PAGE_READER && page != UI_PAGE_TEXT_SETTINGS)
        {
            ui_reader_close();
            reader_session = false;
            reader_return_panel = false;
            reader_confirm_pending = false;
        }
    }
}
