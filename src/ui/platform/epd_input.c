#include "epd_input.h"
#include "epd_app.h"
#include "boards/controls/buttons.h"
#include <rthw.h>
#include <rtm.h>

#define KEY_QUEUE_CAPACITY 32

typedef struct input_scope
{
    struct input_scope *next;
    lv_obj_t *owner, *screen, *back;
    lv_group_t *group;
    const epd_input_ops_t *ops;
} input_scope_t;

static input_scope_t *scopes;
static lv_indev_t *keypad;
static epd_input_event_t key_queue[KEY_QUEUE_CAPACITY];
static unsigned key_head, key_count;
static bool key_overflow, release_pending, dispatch_barrier;
static uint32_t pressed_key;

static bool is_direction(uint32_t key)
{
    return key == LV_KEY_PREV || key == LV_KEY_NEXT;
}

bool epd_input_focusable(lv_obj_t *object)
{
    if (!object || lv_obj_has_state(object, LV_STATE_DISABLED)) return false;
    for (; object; object = lv_obj_get_parent(object))
        if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return false;
    return true;
}

static input_scope_t *scope_for_owner(lv_obj_t *owner)
{
    for (input_scope_t *scope = scopes; scope; scope = scope->next)
        if (scope->owner == owner) return scope;
    return NULL;
}

static void back_deleted(lv_event_t *event)
{
    input_scope_t *scope = lv_event_get_user_data(event);
    if (lv_event_get_target_obj(event) == scope->back) scope->back = NULL;
}

static void scope_deleted(lv_event_t *event)
{
    input_scope_t *scope = lv_event_get_user_data(event);
    if (lv_event_get_target_obj(event) != scope->owner) return;
    input_scope_t **link = &scopes;
    while (*link != scope) link = &(*link)->next;
    *link = scope->next;
    if (scope->back) lv_obj_remove_event_cb_with_user_data(scope->back, back_deleted, scope);
    if (keypad && lv_indev_get_group(keypad) == scope->group)
    {
        lv_indev_reset(keypad, NULL);
        lv_indev_set_group(keypad, NULL);
    }
    lv_group_delete(scope->group);
    lv_free(scope);
}

lv_group_t *epd_app_input_group(lv_obj_t *owner)
{
    input_scope_t *scope = scope_for_owner(owner);
    if (scope) return scope->group;
    scope = lv_malloc_zeroed(sizeof(*scope));
    LV_ASSERT_MALLOC(scope);
    if (!scope) return NULL;
    scope->group = lv_group_create();
    LV_ASSERT_MALLOC(scope->group);
    if (!scope->group)
    {
        lv_free(scope);
        return NULL;
    }
    scope->owner = owner;
    scope->screen = lv_obj_get_screen(owner);
    scope->next = scopes;
    scopes = scope;
    lv_group_set_wrap(scope->group, true);
    lv_obj_add_event_cb(owner, scope_deleted, LV_EVENT_DELETE, scope);
    return scope->group;
}
RTM_EXPORT(epd_app_input_group);

void epd_input_add_object(lv_obj_t *object)
{
    for (lv_obj_t *owner = lv_obj_get_parent(object); owner; owner = lv_obj_get_parent(owner))
    {
        input_scope_t *scope = scope_for_owner(owner);
        if (scope)
        {
            lv_group_add_obj(scope->group, object);
            return;
        }
    }
    lv_group_add_obj(epd_app_input_group(lv_obj_get_screen(object)), object);
}

void epd_app_input_back(lv_obj_t *owner, lv_obj_t *button)
{
    input_scope_t *scope = scope_for_owner(owner);
    if (!scope) return;
    if (scope->back) lv_obj_remove_event_cb_with_user_data(scope->back, back_deleted, scope);
    scope->back = button;
    if (button) lv_obj_add_event_cb(button, back_deleted, LV_EVENT_DELETE, scope);
}
RTM_EXPORT(epd_app_input_back);

void epd_input_set_ops(lv_group_t *group, const epd_input_ops_t *ops)
{
    for (input_scope_t *scope = scopes; scope; scope = scope->next)
        if (scope->group == group)
        {
            scope->ops = ops;
            return;
        }
}

static void action_post(UIAction action)
{
    uint32_t key;
    switch (action)
    {
    case UP: key = LV_KEY_PREV; break;
    case DOWN: key = LV_KEY_NEXT; break;
    case SELECT: key = LV_KEY_ENTER; break;
    case UPGLIDE: key = LV_KEY_ESC; break;
    default: return;
    }
    /* The ADC callback records input without touching LVGL. */
    rt_base_t level = rt_hw_interrupt_disable();
    if (key_count && is_direction(key))
    {
        epd_input_event_t *tail = &key_queue[(key_head + key_count - 1) % KEY_QUEUE_CAPACITY];
        if (tail->key == key && tail->repeat < UINT16_MAX)
        {
            ++tail->repeat;
            rt_hw_interrupt_enable(level);
            return;
        }
    }
    if (key_count < KEY_QUEUE_CAPACITY)
    {
        epd_input_event_t *tail = &key_queue[(key_head + key_count) % KEY_QUEUE_CAPACITY];
        tail->key = key;
        tail->repeat = 1;
        ++key_count;
    }
    else key_overflow = true;
    rt_hw_interrupt_enable(level);
}

static void move_focus(lv_group_t *group, const epd_input_event_t *events, unsigned count)
{
    unsigned eligible = 0, total = lv_group_get_obj_count(group);
    int target = 0;
    lv_obj_t *focused = lv_group_get_focused(group);
    for (unsigned i = 0; i < total; ++i)
    {
        lv_obj_t *item = lv_group_get_obj_by_index(group, i);
        if (!epd_input_focusable(item)) continue;
        if (item == focused) target = (int)eligible;
        ++eligible;
    }
    if (!eligible) return;
    for (unsigned i = 0; i < count; ++i)
    {
        int step = events[i].repeat % eligible;
        target += events[i].key == LV_KEY_PREV ? -step : step;
        target = (target + (int)eligible) % (int)eligible;
    }
    for (unsigned i = 0; i < total; ++i)
    {
        lv_obj_t *item = lv_group_get_obj_by_index(group, i);
        if (!epd_input_focusable(item)) continue;
        if (target-- == 0)
        {
            /* Only the final focus invalidates the e-paper UI. */
            if (item != focused) lv_group_focus_obj(item);
            return;
        }
    }
}

static void read_key(lv_indev_t *indev, lv_indev_data_t *data)
{
    data->key = pressed_key;
    data->state = LV_INDEV_STATE_RELEASED;
    if (release_pending)
    {
        release_pending = false;
        return;
    }
    if (!epd_app_refresh_done()) return;
    lv_obj_t *screen = lv_screen_active();
    input_scope_t *scope;
    /* Newer scopes on the active screen are modal; app_fwk owns navigation. */
    for (scope = scopes; scope; scope = scope->next)
        if (scope->screen == lv_layer_top() && epd_input_focusable(scope->owner)) break;
    if (!scope)
        for (scope = scopes; scope; scope = scope->next)
            if (scope->screen == screen && epd_input_focusable(scope->owner)) break;
    if (lv_indev_get_group(indev) != (scope ? scope->group : NULL))
    {
        lv_indev_reset(indev, NULL);
        lv_indev_set_group(indev, scope ? scope->group : NULL);
    }
    if (!scope) return;

    rt_base_t level = rt_hw_interrupt_disable();
    bool pending = key_count != 0;
    uint32_t first = key_queue[key_head].key;
    rt_hw_interrupt_enable(level);
    if (!pending || (scope->ops && scope->ops->ready && !scope->ops->ready(first))) return;

    epd_input_event_t batch[KEY_QUEUE_CAPACITY];
    unsigned count = 0;
    level = rt_hw_interrupt_disable();
    bool overflow = key_overflow;
    key_overflow = false;
    while (key_count && count < KEY_QUEUE_CAPACITY)
    {
        epd_input_event_t event = key_queue[key_head];
        bool direction = is_direction(event.key);
        /* Confirmation and back separate directional batches. */
        if (count && !direction) break;
        batch[count++] = event;
        key_head = (key_head + 1) % KEY_QUEUE_CAPACITY;
        --key_count;
        if (!direction) break;
    }
    rt_hw_interrupt_enable(level);
    if (overflow) rt_kprintf("[ui] key queue full; newest input rejected\n");
    if (!count) return;
    if (is_direction(first))
    {
        if (!scope->ops || !scope->ops->move || !scope->ops->move(batch, count))
            move_focus(scope->group, batch, count);
        return;
    }
    if (scope->ops && scope->ops->key && scope->ops->key(first)) return;
    dispatch_barrier = true;
    if (first == LV_KEY_ESC)
    {
        if (scope->back)
        {
            if (epd_input_focusable(scope->back))
                lv_obj_send_event(scope->back, LV_EVENT_CLICKED, NULL);
        }
        else if (scope->owner == scope->screen) gui_app_goback();
        return;
    }
    if (!epd_input_focusable(lv_group_get_focused(scope->group))) return;
    pressed_key = data->key = first;
    data->state = LV_INDEV_STATE_PRESSED;
    release_pending = true;
}

rt_err_t epd_input_init(void)
{
    keypad = lv_indev_create();
    if (!keypad) return -RT_ENOMEM;
    lv_indev_set_type(keypad, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(keypad, read_key);
    lv_indev_set_mode(keypad, LV_INDEV_MODE_EVENT);
    buttons_init(action_post);
    return RT_EOK;
}

void epd_input_clear(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    key_head = key_count = 0;
    key_overflow = false;
    rt_hw_interrupt_enable(level);
    release_pending = false;
    dispatch_barrier = false;
    lv_indev_reset(keypad, NULL);
}

void epd_input_process(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool pending = key_count != 0;
    rt_hw_interrupt_enable(level);
    if (!pending && !release_pending) return;
    lv_indev_read(keypad);
    /* Event-mode LVGL reads one state at a time. Deliver a complete click. */
    if (release_pending) lv_indev_read(keypad);
    if (dispatch_barrier && !release_pending)
    {
        dispatch_barrier = false;
        /* Run queued navigation after LVGL callbacks return, before the next key. */
        gui_app_exec_now();
    }
}

RTM_EXPORT(epd_input_focusable);
RTM_EXPORT(epd_input_set_ops);
