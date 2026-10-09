#include "ui_app.h"
#include <rthw.h>
#include "launcher.h"
#include "platform/epd_input.h"
#include "boards/battery/battery.h"
#include "bt_pan.h"
#include "app_service.h"
#include <string.h>
#include <time.h>

static launcher_status_t published;
static rt_tick_t last_status_tick;

void ui_app_set_wifi(bool enabled, bool connected)
{
    rt_base_t level = rt_hw_interrupt_disable();
    published.wifi = ui_radio_state(enabled, connected);
    rt_hw_interrupt_enable(level);
}

void ui_app_set_recent_reading(const char *title, int percent)
{
    rt_base_t level = rt_hw_interrupt_disable();
    rt_strncpy(published.recent_book, title ? title : "", sizeof(published.recent_book) - 1);
    published.recent_book[sizeof(published.recent_book) - 1] = '\0';
    published.reading_percent = ui_battery_percent(percent);
    rt_hw_interrupt_enable(level);
}

static void status_poll(void)
{
    launcher_status_t snapshot;
    rt_base_t level = rt_hw_interrupt_disable();
    snapshot = published;
    btpan_state_t pan_state = btpan_get_state();
    snapshot.bluetooth = ui_radio_state(
        pan_state != BTPAN_STATE_OFF,
        pan_state == BTPAN_STATE_CONNECTED || pan_state == BTPAN_STATE_NETWORK_READY);
    rt_hw_interrupt_enable(level);
    app_service_summary("weather", snapshot.weather, sizeof(snapshot.weather));
    snapshot.battery_percent = battery_get_percentage();
    snapshot.charging = battery_is_charging();
    time_t now = time(RT_NULL);
    struct tm calendar;
    if (localtime_r(&now, &calendar) && calendar.tm_year >= 124 && calendar.tm_year < 200)
    {
        strftime(snapshot.time_text, sizeof(snapshot.time_text), "%Y-%m-%d %H:%M", &calendar);
        strftime(snapshot.clock_text, sizeof(snapshot.clock_text), "%H:%M", &calendar);
    }
    else
    {
        rt_strncpy(snapshot.time_text, "---- -- --  --:--", sizeof(snapshot.time_text));
        rt_strncpy(snapshot.clock_text, "--:--", sizeof(snapshot.clock_text));
    }
    launcher_set_status(&snapshot);
}

rt_err_t ui_app_init(void)
{
    if (!launcher_init()) return -RT_ERROR;
    battery_init();
    status_poll();
    last_status_tick = rt_tick_get();
    return epd_input_init();
}

void ui_app_process(void)
{
    if (!launcher_storage_process())
    {
        epd_input_clear();
        return;
    }
    epd_input_process();
    launcher_process();
    rt_tick_t tick = rt_tick_get();
    if ((rt_tick_t)(tick - last_status_tick) >= rt_tick_from_millisecond(1000))
    {
        status_poll();
        last_status_tick = tick;
    }
}
