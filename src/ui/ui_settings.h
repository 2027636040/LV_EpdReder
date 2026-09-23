#ifndef UI_SETTINGS_H
#define UI_SETTINGS_H

#include <stdbool.h>

typedef enum
{
    UI_SETTING_TOUCH,
    UI_SETTING_TIMEOUT,
    UI_SETTING_FULL_REFRESH,
    UI_SETTING_BLUETOOTH,
    UI_SETTING_WIFI,
    UI_SETTING_NETWORK,
    UI_SETTING_LOW_POWER,
    UI_SETTING_FONT,
    UI_SETTING_FONT_SIZE,
    UI_SETTING_FONT_WEIGHT,
    UI_SETTING_LINE_SPACING,
    UI_SETTING_MARGIN,
    UI_SETTING_ENCODING,
    UI_SETTING_COUNT
} ui_setting_id_t;

typedef struct
{
    const char *label;
    const char *const *options;
    unsigned option_count;
    bool is_switch;
} ui_setting_t;

const ui_setting_t *ui_settings_item(ui_setting_id_t id);
const char *ui_settings_value(ui_setting_id_t id);
bool ui_settings_enabled(ui_setting_id_t id);
void ui_settings_cycle(ui_setting_id_t id);
unsigned ui_settings_index(ui_setting_id_t id);
unsigned ui_settings_refresh_count(void);

#endif
