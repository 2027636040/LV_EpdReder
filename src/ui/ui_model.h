#ifndef UI_MODEL_H
#define UI_MODEL_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    UI_PAGE_HOME, UI_PAGE_FILES,
    UI_PAGE_SETTINGS, UI_PAGE_TEXT_SETTINGS, UI_PAGE_ABOUT, UI_PAGE_LOCK,
    UI_PAGE_DISPLAY_SETTINGS, UI_PAGE_NETWORK_SETTINGS, UI_PAGE_WIFI_SETTINGS, UI_PAGE_STORAGE,
    UI_PAGE_APP_MANAGEMENT, UI_PAGE_APP_INSTALL, UI_PAGE_APP_UNINSTALL,
    UI_PAGE_APP_SETTINGS, UI_PAGE_MEMORY, UI_PAGE_COUNT
} ui_page_id_t;

typedef enum
{
    UI_RADIO_OFF, UI_RADIO_DISCONNECTED, UI_RADIO_CONNECTED
} ui_radio_state_t;

typedef enum
{
    UI_BATTERY_EMPTY, UI_BATTERY_MID, UI_BATTERY_FULL, UI_BATTERY_CHARGING
} ui_battery_icon_t;

ui_radio_state_t ui_radio_state(bool enabled, bool connected);
int ui_battery_percent(int percent);
ui_battery_icon_t ui_battery_icon(int percent, bool charging);

#endif
