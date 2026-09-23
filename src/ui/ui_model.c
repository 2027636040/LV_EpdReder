#include "ui_model.h"
#include <string.h>

ui_radio_state_t ui_radio_state(bool enabled, bool connected)
{
    return !enabled ? UI_RADIO_OFF : connected ? UI_RADIO_CONNECTED : UI_RADIO_DISCONNECTED;
}

int ui_battery_percent(int percent)
{
    if (percent < 0) return 0;
    return percent > 100 ? 100 : percent;
}

ui_battery_icon_t ui_battery_icon(int percent, bool charging)
{
    if (charging) return UI_BATTERY_CHARGING;
    percent = ui_battery_percent(percent);
    if (percent <= 20) return UI_BATTERY_EMPTY;
    if (percent < 80) return UI_BATTERY_MID;
    return UI_BATTERY_FULL;
}

void ui_nav_init(ui_nav_t *nav)
{
    memset(nav, 0, sizeof(*nav));
    nav->depth = 1;
    nav->entries[0].page = UI_PAGE_HOME;
}

ui_nav_entry_t *ui_nav_current(ui_nav_t *nav)
{
    return &nav->entries[nav->depth - 1];
}

bool ui_nav_push(ui_nav_t *nav, ui_page_id_t page)
{
    if (page < UI_PAGE_HOME || page >= UI_PAGE_COUNT ||
        ui_nav_current(nav)->page == page) return false;
    if (page == UI_PAGE_HOME)
    {
        nav->depth = 1;
        return true;
    }
    if (nav->depth >= UI_NAV_CAPACITY) return false;
    nav->entries[nav->depth++] = (ui_nav_entry_t){page, 0};
    return true;
}

bool ui_nav_back(ui_nav_t *nav)
{
    if (nav->depth <= 1) return false;
    nav->depth--;
    return true;
}
