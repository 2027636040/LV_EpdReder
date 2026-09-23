#include "ui_settings.h"
#include <stddef.h>
#include <stdint.h>

static const char *const timeouts[] = {"1 分钟", "3 分钟", "5 分钟", "10 分钟", "15 分钟"};
static const char *const refresh_cycles[] = {"每次", "5 次", "8 次", "10 次", "15 次", "20 次"};
static const char *const networks[] = {"WiFi", "BLE-PAN"};
static const char *const fonts[] = {"Default", "Song", "Hei", "Kai", "Monospace"};
static const char *const font_sizes[] = {"16 px", "18 px", "20 px", "22 px", "24 px", "28 px", "32 px", "36 px"};
static const char *const font_weights[] = {"正常", "粗体", "细体"};
static const char *const line_spacings[] = {"1.0x", "1.2x", "1.4x", "1.6x", "1.8x"};
static const char *const margins[] = {"4 px", "8 px", "12 px", "16 px", "20 px"};
static const char *const encodings[] = {"自动", "UTF-8", "GBK"};

#define CHOICE(label, values) {label, values, sizeof(values) / sizeof(values[0]), false}
#define SWITCH(label) {label, NULL, 2, true}

static const ui_setting_t settings[UI_SETTING_COUNT] = {
    [UI_SETTING_TOUCH] = SWITCH("触控开关："),
    [UI_SETTING_TIMEOUT] = CHOICE("超时关机：", timeouts),
    [UI_SETTING_FULL_REFRESH] = CHOICE("全刷周期：", refresh_cycles),
    [UI_SETTING_BLUETOOTH] = SWITCH("蓝牙："),
    [UI_SETTING_WIFI] = SWITCH("WIFI："),
    [UI_SETTING_NETWORK] = CHOICE("默认网络：", networks),
    [UI_SETTING_LOW_POWER] = SWITCH("低功耗模式："),
    [UI_SETTING_FONT] = CHOICE("字体：", fonts),
    [UI_SETTING_FONT_SIZE] = CHOICE("字号：", font_sizes),
    [UI_SETTING_FONT_WEIGHT] = CHOICE("字重：", font_weights),
    [UI_SETTING_LINE_SPACING] = CHOICE("行距：", line_spacings),
    [UI_SETTING_MARGIN] = CHOICE("边距：", margins),
    [UI_SETTING_ENCODING] = CHOICE("文件编码：", encodings)
};

/* Selections are kept for the current UI session. */
static uint8_t selected[UI_SETTING_COUNT] = {
    [UI_SETTING_TOUCH] = 1,
    [UI_SETTING_TIMEOUT] = 2,
    [UI_SETTING_FULL_REFRESH] = 3,
    [UI_SETTING_WIFI] = 1,
    [UI_SETTING_LOW_POWER] = 1,
    [UI_SETTING_FONT_SIZE] = 4,
    [UI_SETTING_LINE_SPACING] = 1,
    [UI_SETTING_MARGIN] = 1
};

const ui_setting_t *ui_settings_item(ui_setting_id_t id)
{
    return (unsigned)id < UI_SETTING_COUNT ? &settings[id] : NULL;
}

const char *ui_settings_value(ui_setting_id_t id)
{
    const ui_setting_t *setting = ui_settings_item(id);
    return setting && !setting->is_switch ? setting->options[selected[id]] : NULL;
}

bool ui_settings_enabled(ui_setting_id_t id)
{
    const ui_setting_t *setting = ui_settings_item(id);
    return setting && setting->is_switch && selected[id] != 0;
}

void ui_settings_cycle(ui_setting_id_t id)
{
    const ui_setting_t *setting = ui_settings_item(id);
    if (setting) selected[id] = (selected[id] + 1) % setting->option_count;
}

unsigned ui_settings_index(ui_setting_id_t id)
{
    return (unsigned)id < UI_SETTING_COUNT ? selected[id] : 0;
}

unsigned ui_settings_refresh_count(void)
{
    static const unsigned cycles[] = {1, 5, 8, 10, 15, 20};
    return cycles[selected[UI_SETTING_FULL_REFRESH]];
}
