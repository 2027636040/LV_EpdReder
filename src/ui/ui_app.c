#include "ui_app.h"
#include <rthw.h>
#include "launcher.h"
#include "boards/controls/buttons.h"
#include "boards/battery/battery.h"
#include "bt_pan.h"
#include <string.h>
#include <time.h>

#define KEY_QUEUE_CAPACITY 32

static rt_mq_t battery_events;
static launcher_key_event_t key_queue[KEY_QUEUE_CAPACITY];
static unsigned key_head, key_count;
static bool key_overflow;
static launcher_status_t published;
static rt_tick_t last_status_tick;
static bool first_poll = true;

static void action_post(UIAction action)
{
    launcher_key_t key;
    switch (action)
    {
    case UP: key = LAUNCHER_KEY_PREVIOUS; break;
    case DOWN: key = LAUNCHER_KEY_NEXT; break;
    case SELECT: key = LAUNCHER_KEY_ENTER; break;
    case UPGLIDE: key = LAUNCHER_KEY_BACK; break;
    default: return;
    }

    /* The button callback only records input, even while the UI waits for LCD DMA. */
    rt_base_t level = rt_hw_interrupt_disable();
    if (key_count && (key == LAUNCHER_KEY_PREVIOUS || key == LAUNCHER_KEY_NEXT))
    {
        launcher_key_event_t *tail = &key_queue[(key_head + key_count - 1) % KEY_QUEUE_CAPACITY];
        if (tail->key == key && tail->repeat < UINT16_MAX)
        {
            ++tail->repeat;
            rt_hw_interrupt_enable(level);
            return;
        }
    }
    if (key_count < KEY_QUEUE_CAPACITY)
    {
        launcher_key_event_t *tail = &key_queue[(key_head + key_count) % KEY_QUEUE_CAPACITY];
        tail->key = key;
        tail->repeat = 1;
        ++key_count;
    }
    else
        key_overflow = true;
    rt_hw_interrupt_enable(level);
}

static void keys_process(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool pending = key_count != 0;
    launcher_key_t first = key_queue[key_head].key;
    rt_hw_interrupt_enable(level);
    if (!pending || !launcher_input_ready(first)) return;
    launcher_key_event_t batch[KEY_QUEUE_CAPACITY];
    unsigned count = 0;
    level = rt_hw_interrupt_disable();
    bool overflow = key_overflow;
    key_overflow = false;
    while (key_count && count < KEY_QUEUE_CAPACITY)
    {
        launcher_key_event_t event = key_queue[key_head];
        bool direction = event.key == LAUNCHER_KEY_PREVIOUS || event.key == LAUNCHER_KEY_NEXT;
        /* Confirm/back are barriers: neither combine across them nor consume their successors. */
        if (count && !direction) break;
        batch[count++] = event;
        key_head = (key_head + 1) % KEY_QUEUE_CAPACITY;
        --key_count;
        if (!direction) break;
    }
    rt_hw_interrupt_enable(level);
    if (overflow) rt_kprintf("[ui] key queue full; newest input rejected\n");
    if (!count) return;
    if (batch[0].key == LAUNCHER_KEY_PREVIOUS || batch[0].key == LAUNCHER_KEY_NEXT)
        launcher_move(batch, count);
    else
        launcher_key(batch[0].key);
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
        pan_state != BTPAN_STATE_OFF,
        pan_state == BTPAN_STATE_CONNECTED || pan_state == BTPAN_STATE_NETWORK_READY);
    rt_hw_interrupt_enable(level);
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
    battery_events = rt_mq_create("ui_bat", sizeof(UIAction), 16, RT_IPC_FLAG_FIFO);
    if (!battery_events) return -RT_ENOMEM;
    if (!launcher_init())
    {
        rt_mq_delete(battery_events);
        battery_events = RT_NULL;
        return -RT_ENOMEM;
    }
    battery_init(battery_events);
    status_poll();
    first_poll = false;
    last_status_tick = rt_tick_get();
    buttons_init(action_post);
    return RT_EOK;
}

void ui_app_process(void)
{
    UIAction action;
    while (rt_mq_recv(battery_events, &action, sizeof(action), 0) == RT_EOK)
    {
        if (action == MSG_BATTERY_CHECK || action == MSG_UPDATE_CHARGE_STATUS)
            first_poll = true;
    }
    keys_process();
    launcher_process();
    rt_tick_t tick = rt_tick_get();
    if (first_poll || (rt_tick_t)(tick - last_status_tick) >= rt_tick_from_millisecond(1000))
    {
        status_poll();
        last_status_tick = tick;
        first_poll = false;
    }
}
