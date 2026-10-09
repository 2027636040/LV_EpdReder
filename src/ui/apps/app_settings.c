#include "ui_internal.h"
#include "platform/app_card.h"
#include "storage.h"

#define LOAD_CARD_APPS (-198)

static void setting_render(ui_setting_id_t id);
static void setting_row_create(lv_obj_t *screen, ui_setting_id_t id, int y, int height);
static void setting_link_create(lv_obj_t *screen, const char *text, int y, ui_page_id_t page);


void settings_resume(void)
{
    for (unsigned i = 0; i < UI_SETTING_COUNT; ++i) setting_render((ui_setting_id_t)i);
}


bool settings_action(int *request)
{
    int target = *request;
    if (launcher_current_page() == UI_PAGE_APP_MANAGEMENT && target == LOAD_CARD_APPS)
    {
        if (!storage_available(STORAGE_SD)) popup_open("请插入 TF 卡", NULL);
        else app_card_request();
        return true;
    }
    if (launcher_current_page() == UI_PAGE_WIFI_SETTINGS ||
        launcher_current_page() == UI_PAGE_DISPLAY_SETTINGS ||
        launcher_current_page() == UI_PAGE_NETWORK_SETTINGS || launcher_current_page() == UI_PAGE_TEXT_SETTINGS)
    {
        if (target >= SETTING_CHANGE_BASE && target < SETTING_CHANGE_BASE + UI_SETTING_COUNT)
        {
            ui_setting_id_t id = (ui_setting_id_t)(target - SETTING_CHANGE_BASE);
            if ((id >= UI_SETTING_FONT) != (launcher_current_page() == UI_PAGE_TEXT_SETTINGS)) return true;
            if (!ui_settings_cycle(id))
            {
                popup_open("设置失败，请重试", NULL);
                return true;
            }
            setting_render(id);
            return true;
        }
    }

    return false;
}

void settings_process(void)
{
    app_list_process();
    if ((launcher_current_page() == UI_PAGE_DISPLAY_SETTINGS ||
         launcher_current_page() == UI_PAGE_NETWORK_SETTINGS) && !current->popup && lv_refreshing_done())
    {
        setting_render(UI_SETTING_BLUETOOTH);
        setting_render(UI_SETTING_FULL_REFRESH);
    }

}

static int app_main(intent_t intent)
{
    (void)intent;
    return ui_navigation_start("settings", UI_PAGE_SETTINGS);
}
BUILTIN_APP_EXPORT("设置", &ui_icon_settings, "settings", app_main, 1);


static void setting_render(ui_setting_id_t id)
{
    setting_view_t *setting = &current->setting_views[id];
    if (setting->track)
    {
        bool enabled = ui_settings_enabled(id);
        if (setting->rendered && setting->enabled == enabled) return;
        lv_obj_set_style_bg_color(setting->track,
                                  lv_color_hex(enabled ? 0x555555 : 0xBBBBBB), 0);
        lv_obj_set_x(setting->knob, enabled ? 40 : 4);
        setting->enabled = enabled;
        setting->rendered = true;
    }
    else
        text_update(setting->value, ui_settings_value(id));
}

static void setting_row_create(lv_obj_t *screen, ui_setting_id_t id, int y, int height)
{
    const ui_setting_t *setting = ui_settings_item(id);
    setting_view_t *item = &current->setting_views[id];
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

void settings_create(lv_obj_t *screen)
{
    page_title_create(screen, "设置", ui_font_title());
    setting_link_create(screen, "显示设置", 194, UI_PAGE_DISPLAY_SETTINGS);
    setting_link_create(screen, "无线和网络", 302, UI_PAGE_NETWORK_SETTINGS);
    setting_link_create(screen, "存储管理", 410, UI_PAGE_STORAGE);
    setting_link_create(screen, "内存使用", 518, UI_PAGE_MEMORY);
    setting_link_create(screen, "应用管理", 626, UI_PAGE_APP_MANAGEMENT);
    setting_link_create(screen, "关于设备", 734, UI_PAGE_ABOUT);
}

void display_settings_create(lv_obj_t *screen)
{
    page_title_create(screen, "显示设置", ui_font_title());
    setting_row_create(screen, UI_SETTING_FULL_REFRESH, 194, 80);
    setting_link_create(screen, "字体设置", 302, UI_PAGE_TEXT_SETTINGS);
    setting_row_create(screen, UI_SETTING_TIMEOUT, 410, 80);
}

void network_settings_create(lv_obj_t *screen)
{
    page_title_create(screen, "无线和网络", ui_font_title());
    setting_row_create(screen, UI_SETTING_WIFI, 194, 80);
    setting_row_create(screen, UI_SETTING_BLUETOOTH, 302, 80);
    setting_row_create(screen, UI_SETTING_NETWORK, 410, 80);
    setting_link_create(screen, "WiFi 配网", 518, UI_PAGE_WIFI_SETTINGS);
}

void wifi_settings_create(lv_obj_t *screen)
{
    page_title_create(screen, "WiFi 配网", ui_font_title());
    setting_row_create(screen, UI_SETTING_WIFI, 194, 80);
    centered_label(screen, "未接入 WiFi 设备", MARGIN, 420, CONTENT_WIDTH, ui_font_body());
}

void app_management_create(lv_obj_t *screen)
{
    page_title_create(screen, "应用管理", ui_font_title());
    setting_link_create(screen, "应用安装", 194, UI_PAGE_APP_INSTALL);
    setting_link_create(screen, "应用卸载", 302, UI_PAGE_APP_UNINSTALL);
    setting_link_create(screen, "应用设置", 410, UI_PAGE_APP_SETTINGS);
    button_create(screen, MARGIN, 518, CONTENT_WIDTH, 88, "加载 TF 卡应用", LOAD_CARD_APPS);
}

void text_settings_create(lv_obj_t *screen)
{
    page_title_create(screen, "字体设置", ui_font_title());
    for (unsigned i = UI_SETTING_FONT; i < UI_SETTING_COUNT; ++i)
        setting_row_create(screen, (ui_setting_id_t)i, 174 + (i - UI_SETTING_FONT) * 132, 110);
}

void about_create(lv_obj_t *screen)
{
    page_title_create(screen, "关于设备", ui_font_title());
    label_create(screen, "SiFli EPD Reader", MARGIN, 210, CONTENT_WIDTH, ui_font_title());
    label_create(screen, "处理器：SF32LB57", MARGIN, 304, CONTENT_WIDTH, ui_font_body());
    label_create(screen, "屏幕：E0470A03", MARGIN, 372, CONTENT_WIDTH, ui_font_body());
    label_create(screen, "分辨率：684 × 1216", MARGIN, 440, CONTENT_WIDTH, ui_font_body());
    char version[64];
    snprintf(version, sizeof(version), "LVGL：%d.%d.%d", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    label_create(screen, version, MARGIN, 508, CONTENT_WIDTH, ui_font_body());
    snprintf(version, sizeof(version), "RT-Thread：%d.%d.%d", RT_VERSION, RT_SUBVERSION, RT_REVISION);
    label_create(screen, version, MARGIN, 576, CONTENT_WIDTH, ui_font_body());
}
