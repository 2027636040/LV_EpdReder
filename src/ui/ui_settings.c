#include "ui_settings.h"
#include "ui_settings_store.h"
#include "bt_pan.h"
#include "boards/epd_e0470a03_57x/epd_waveform.h"
#include <stddef.h>
#include <stdint.h>

static const char *const timeouts[] = {"1 分钟", "3 分钟", "5 分钟", "10 分钟", "15 分钟"};
static const char *const refresh_cycles[] = {"每次", "5 次", "8 次", "10 次", "15 次", "20 次"};
static const unsigned refresh_counts[] = {1, 5, 8, 10, 15, 20};
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

/* Accessed only by the application/UI thread. */
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
    if (id == UI_SETTING_FULL_REFRESH)
    {
        static char value[24];
        unsigned count = ui_settings_refresh_count();
        if (count == 1) return refresh_cycles[0];
        rt_snprintf(value, sizeof(value), "%u 次", count);
        return value;
    }
    return setting && !setting->is_switch ? setting->options[selected[id]] : NULL;
}

bool ui_settings_enabled(ui_setting_id_t id)
{
    if (id == UI_SETTING_BLUETOOTH) return btpan_get_state() != BTPAN_STATE_OFF;
    const ui_setting_t *setting = ui_settings_item(id);
    return setting && setting->is_switch && selected[id] != 0;
}

void ui_settings_init(void)
{
    uint8_t saved[UI_SETTING_COUNT];
    selected[UI_SETTING_BLUETOOTH] = ui_settings_enabled(UI_SETTING_BLUETOOTH);
    if (ui_settings_store_load(saved))
    {
        bool valid = true;
        for (unsigned i = 0; i < UI_SETTING_COUNT; ++i)
            if (saved[i] >= settings[i].option_count) valid = false;
        if (valid)
        {
            for (unsigned i = 0; i < UI_SETTING_COUNT; ++i) selected[i] = saved[i];
            btpan_enable(selected[UI_SETTING_BLUETOOTH] != 0);
        }
    }
    epd_wave_set_part_times(refresh_counts[selected[UI_SETTING_FULL_REFRESH]]);
}

bool ui_settings_cycle(ui_setting_id_t id)
{
    const ui_setting_t *setting = ui_settings_item(id);
    if (!setting) return false;
    unsigned next = (ui_settings_index(id) + 1) % setting->option_count;
    if (id == UI_SETTING_BLUETOOTH && btpan_enable(next != 0) != RT_EOK) return false;
    if (id == UI_SETTING_FULL_REFRESH) epd_wave_set_part_times(refresh_counts[next]);
    selected[id] = next;
    return true;
}

bool ui_settings_save(void)
{
    selected[UI_SETTING_BLUETOOTH] = ui_settings_enabled(UI_SETTING_BLUETOOTH);
    selected[UI_SETTING_FULL_REFRESH] = ui_settings_index(UI_SETTING_FULL_REFRESH);
    return ui_settings_store_save(selected);
}

unsigned ui_settings_index(ui_setting_id_t id)
{
    if (id == UI_SETTING_BLUETOOTH) return ui_settings_enabled(id);
    if (id == UI_SETTING_FULL_REFRESH)
    {
        unsigned count = ui_settings_refresh_count();
        for (unsigned i = 0; i < sizeof(refresh_counts) / sizeof(refresh_counts[0]); ++i)
            if (refresh_counts[i] == count) return i;
    }
    return (unsigned)id < UI_SETTING_COUNT ? selected[id] : 0;
}

unsigned ui_settings_refresh_count(void)
{
    return (unsigned)epd_wave_get_part_times();
}
