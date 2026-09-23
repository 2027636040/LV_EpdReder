#include "ui_app.h"
#include <rthw.h>
#include "launcher.h"
#include "boards/controls/buttons.h"
#include "boards/battery/battery.h"
#include "bt_pan.h"
#include <string.h>
#include <time.h>

static rt_mq_t actions;
static launcher_status_t published;
static rt_tick_t last_status_tick;
static bool first_poll = true;

static void action_post(UIAction action)
{
    if (actions) rt_mq_send(actions, &action, sizeof(action));
}

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

void ui_app_set_weather_summary(const char *summary)
{
    rt_base_t level = rt_hw_interrupt_disable();
    rt_strncpy(published.weather, summary ? summary : "", sizeof(published.weather) - 1);
    published.weather[sizeof(published.weather) - 1] = '\0';
    rt_hw_interrupt_enable(level);
}

static void status_poll(void)
{
    launcher_status_t snapshot;
    rt_base_t level = rt_hw_interrupt_disable();
    snapshot = published;
    btpan_state_t pan_state = btpan_get_state();
    snapshot.bluetooth = ui_radio_state(
        pan_state != BTPAN_STATE_OFF && pan_state != BTPAN_STATE_INITIALIZING,
        pan_state == BTPAN_STATE_CONNECTED || pan_state == BTPAN_STATE_NETWORK_READY);
    rt_hw_interrupt_enable(level);
    snapshot.battery_percent = battery_get_percentage();
    snapshot.charging = battery_is_charging();
    time_t now = time(RT_NULL);
    struct tm calendar;
    if (localtime_r(&now, &calendar) && calendar.tm_year >= 124 && calendar.tm_year < 200)
        strftime(snapshot.time_text, sizeof(snapshot.time_text), "%Y-%m-%d %H:%M", &calendar);
    else
        rt_strncpy(snapshot.time_text, "---- -- --  --:--", sizeof(snapshot.time_text));
    launcher_set_status(&snapshot);
}

rt_err_t ui_app_init(void)
{
    actions = rt_mq_create("ui_keys", sizeof(UIAction), 16, RT_IPC_FLAG_FIFO);
    if (!actions) return -RT_ENOMEM;
    if (!launcher_init())
    {
        rt_mq_delete(actions);
        actions = RT_NULL;
        return -RT_ENOMEM;
    }
    battery_init(actions);
    status_poll();
    first_poll = false;
    last_status_tick = rt_tick_get();
    buttons_init(action_post);
    return RT_EOK;
}

void ui_app_process(void)
{
    UIAction action;
    bool reading = launcher_current_page() == UI_PAGE_READER;
    bool key_handled = false;
    while (rt_mq_recv(actions, &action, sizeof(action), 0) == RT_EOK)
    {
        if (action == MSG_BATTERY_CHECK || action == MSG_UPDATE_CHARGE_STATUS)
        {
            first_poll = true;
            continue;
        }
        if (reading && key_handled) continue;
        key_handled = true;
        switch (action)
        {
        case UP: launcher_key(LAUNCHER_KEY_PREVIOUS); break;
        case DOWN: launcher_key(LAUNCHER_KEY_NEXT); break;
        case SELECT: launcher_key(LAUNCHER_KEY_ENTER); break;
        case UPGLIDE: launcher_key(LAUNCHER_KEY_BACK); break;
        case MSG_BATTERY_CHECK: case MSG_UPDATE_CHARGE_STATUS: first_poll = true; break;
        default: break;
        }
        if (!reading) break;
    }
    launcher_process();
    rt_tick_t tick = rt_tick_get();
    if (first_poll || (rt_tick_t)(tick - last_status_tick) >= rt_tick_from_millisecond(1000))
    {
        status_poll();
        last_status_tick = tick;
        first_poll = false;
    }
}
