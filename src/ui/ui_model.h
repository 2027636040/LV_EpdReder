#ifndef UI_MODEL_H
#define UI_MODEL_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    UI_PAGE_HOME, UI_PAGE_BOOKSHELF, UI_PAGE_WEATHER, UI_PAGE_TRANSFER,
    UI_PAGE_ALBUM, UI_PAGE_WIFI, UI_PAGE_SETTINGS, UI_PAGE_READER,
    UI_PAGE_TEXT_SETTINGS, UI_PAGE_CITY, UI_PAGE_ABOUT, UI_PAGE_LOCK,
    UI_PAGE_WEATHER_SETTINGS, UI_PAGE_CITY_INPUT, UI_PAGE_COUNT
} ui_page_id_t;

typedef enum
{
    UI_RADIO_OFF, UI_RADIO_DISCONNECTED, UI_RADIO_CONNECTED
} ui_radio_state_t;

typedef enum
{
    UI_BATTERY_EMPTY, UI_BATTERY_MID, UI_BATTERY_FULL, UI_BATTERY_CHARGING
} ui_battery_icon_t;

#define UI_NAV_CAPACITY 8
typedef struct
{
    ui_page_id_t page;
    uint8_t focus;
} ui_nav_entry_t;

typedef struct
{
    ui_nav_entry_t entries[UI_NAV_CAPACITY];
    uint8_t depth;
} ui_nav_t;

ui_radio_state_t ui_radio_state(bool enabled, bool connected);
int ui_battery_percent(int percent);
ui_battery_icon_t ui_battery_icon(int percent, bool charging);
void ui_nav_init(ui_nav_t *nav);
bool ui_nav_push(ui_nav_t *nav, ui_page_id_t page);
bool ui_nav_back(ui_nav_t *nav);
ui_nav_entry_t *ui_nav_current(ui_nav_t *nav);

#endif
