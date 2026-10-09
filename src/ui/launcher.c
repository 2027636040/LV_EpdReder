#include "ui_internal.h"
#include "storage.h"
#include "platform/app_catalog.h"
#include "platform/app_installer.h"
#include "platform/epd_app.h"
#include "platform/epd_input.h"
#include "platform/epd_refresh.h"
#include "platform/app_card.h"

static bool page_input_ready(uint32_t key);
static bool page_input_move(const epd_input_event_t *events, unsigned count);
static bool page_input_key(uint32_t key);
static const epd_input_ops_t page_input_ops = {page_input_ready, page_input_move, page_input_key};

/* Only the foreground context is bound between callbacks; app_fwk owns all contexts. */
page_context_t *current;
launcher_status_t status;
static lv_style_t epd_style;
int pending_page = NAV_IDLE;
static bool initialized;
static unsigned saved_focus[UI_PAGE_COUNT];
typedef struct app_header
{
    struct app_header *next;
    lv_obj_t *screen;
    status_view_t view;
} app_header_t;
static app_header_t *app_headers;
static void dynamic_headers_refresh(void);
static void page_lifecycle(ui_page_id_t id, gui_app_msg_type_t message, void *memory);

const char *const titles[UI_PAGE_COUNT] = {
    [UI_PAGE_HOME] = "SiFli EPD DEMO",
    [UI_PAGE_FILES] = "文件管理",
    [UI_PAGE_SETTINGS] = "设置",
    [UI_PAGE_TEXT_SETTINGS] = "字体设置",
    [UI_PAGE_ABOUT] = "关于设备", [UI_PAGE_LOCK] = "锁屏",
    [UI_PAGE_DISPLAY_SETTINGS] = "显示设置", [UI_PAGE_NETWORK_SETTINGS] = "无线和网络",
    [UI_PAGE_STORAGE] = "存储管理", [UI_PAGE_APP_MANAGEMENT] = "应用管理",
    [UI_PAGE_WIFI_SETTINGS] = "WiFi 配网",
    [UI_PAGE_APP_INSTALL] = "应用安装", [UI_PAGE_APP_UNINSTALL] = "应用卸载",
    [UI_PAGE_APP_SETTINGS] = "应用设置", [UI_PAGE_MEMORY] = "内存使用"
};

void epd_obj_init(lv_obj_t *obj)
{
    lv_obj_remove_style_all(obj);
    lv_obj_add_style(obj, &epd_style, LV_PART_MAIN);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC |
                      LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

typedef struct
{
    char *text;
} label_text_t;

static void label_text_delete(lv_event_t *event)
{
    label_text_t *saved = lv_event_get_user_data(event);
    lv_free(saved->text);
    lv_free(saved);
}

void text_update(lv_obj_t *label, const char *text)
{
    if (!label || !text) return;

    label_text_t *saved = NULL;
    for (uint32_t i = 0; i < lv_obj_get_event_count(label); ++i)
    {
        lv_event_dsc_t *event = lv_obj_get_event_dsc(label, i);
        if (lv_event_dsc_get_cb(event) == label_text_delete)
        {
            saved = lv_event_dsc_get_user_data(event);
            break;
        }
    }
    /* LV_LABEL_LONG_DOT can modify the label's copy of the original text. */
    if (saved && strcmp(saved->text, text) == 0) return;

    char *copy = lv_strdup(text);
    if (!copy) return;
    if (!saved)
    {
        saved = lv_malloc(sizeof(*saved));
        if (!saved)
        {
            lv_free(copy);
            return;
        }
        saved->text = NULL;
        if (!lv_obj_add_event_cb(label, label_text_delete, LV_EVENT_DELETE, saved))
        {
            lv_free(copy);
            lv_free(saved);
            return;
        }
    }
    lv_label_set_text(label, copy);
    lv_free(saved->text);
    saved->text = copy;
}

lv_obj_t *label_create(lv_obj_t *parent, const char *text, int x, int y,
                              int width, const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent);
    epd_obj_init(label);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0x222222), 0);
    text_update(label, text);
    lv_obj_set_pos(label, x, y);
    if (width > 0)
    {
        lv_obj_set_width(label, width);
        lv_obj_set_height(label, font->line_height);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    }
    return label;
}

lv_obj_t *icon_create(lv_obj_t *parent, const lv_image_dsc_t *src, int x, int y)
{
    lv_obj_t *icon = lv_image_create(parent);
    epd_obj_init(icon);
    lv_image_set_src(icon, src);
    lv_obj_set_style_image_recolor_opa(icon, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(icon, x, y);
    return icon;
}

lv_obj_t *panel_create(lv_obj_t *parent, int x, int y, int width, int height)
{
    lv_obj_t *panel = lv_obj_create(parent);
    epd_obj_init(panel);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, width, height);
    lv_obj_set_style_bg_color(panel, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x999999), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    return panel;
}

unsigned launcher_focus_index(void)
{
    if (!current || !current->group) return 0;
    lv_obj_t *focused = lv_group_get_focused(current->group);
    for (unsigned i = 0; i < current->focus_count; ++i)
        if (current->focus_items[i] == focused) return i;
    return 0;
}

static void clicked(lv_event_t *event)
{
    if (storage_changing()) return;
    if (!current || !current->foreground ||
        lv_obj_get_screen(lv_event_get_target_obj(event)) != current->screen) return;
    if (pending_page != NAV_IDLE) return;
    int target = (int)(intptr_t)lv_event_get_user_data(event);
    lv_obj_t *button = lv_event_get_target_obj(event);
    lv_group_focus_obj(button);
    pending_page = target;
}

lv_obj_t *button_create(lv_obj_t *parent, int x, int y, int width, int height,
                               const char *text, int target)
{
    lv_obj_t *button = panel_create(parent, x, y, width, height);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_color(button, lv_color_black(), LV_STATE_FOCUSED);
    /* Use an inset outline so focus never changes the content coordinates. */
    lv_obj_set_style_outline_color(button, lv_color_black(), LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(button, 3, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(button, -4, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(button, clicked, LV_EVENT_CLICKED, (void *)(intptr_t)target);
    lv_group_add_obj(current->group, button);
    LV_ASSERT(current->focus_count < FOCUS_CAPACITY);
    current->focus_items[current->focus_count++] = button;
    if (text)
    {
        lv_obj_t *label = label_create(button, text, 0, 0, width - 24, ui_font_body());
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(label);
    }
    return button;
}

lv_obj_t *icon_button_create(lv_obj_t *parent, int x, int y,
                                    const lv_image_dsc_t *src, int target)
{
    lv_obj_t *button = button_create(parent, x, y, ICON_BUTTON_SIZE, ICON_BUTTON_SIZE, NULL, target);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
    lv_obj_t *icon = icon_create(button, src, 0, 0);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(icon);
    return button;
}

lv_obj_t *page_title_create(lv_obj_t *screen, const char *text, const lv_font_t *font)
{
    return label_create(screen, text, PAGE_TITLE_X, PAGE_TITLE_Y, WIDTH - MARGIN - PAGE_TITLE_X, font);
}

void image_update(lv_obj_t *icon, const lv_image_dsc_t *src)
{
    if (lv_image_get_src(icon) != src) lv_image_set_src(icon, src);
}

static int radio_update(lv_obj_t *icon, ui_radio_state_t state, int right,
                         const lv_image_dsc_t *on, const lv_image_dsc_t *disconnected)
{
    if (!icon) return right;
    if (state == UI_RADIO_OFF)
    {
        lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
        return right;
    }
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_HIDDEN);
    image_update(icon, state == UI_RADIO_CONNECTED ? on : disconnected);
    lv_obj_set_x(icon, right - 32);
    return right - 44;
}

static void status_render(status_view_t *view)
{
    char buffer[196];
    text_update(view->time, status.time_text[0] ? status.time_text : "---- -- --  --:--");
    snprintf(buffer, sizeof(buffer), "%d%%", ui_battery_percent(status.battery_percent));
    text_update(view->percent, buffer);
    static const lv_image_dsc_t *const batteries[] = {
        &ui_icon_battery_empty, &ui_icon_battery_mid,
        &ui_icon_battery_full, &ui_icon_battery_charge
    };
    if (view->battery)
        image_update(view->battery, batteries[ui_battery_icon(status.battery_percent, status.charging)]);
    int right = radio_update(view->bluetooth, status.bluetooth, WIDTH - MARGIN - (view->battery ? 140 : 0),
                              &ui_icon_bt_on, &ui_icon_bt_disconnected);
    radio_update(view->wifi, status.wifi, right, &ui_icon_wifi_on, &ui_icon_wifi_disconnected);
    if (view->recent)
    {
        if (status.recent_book[0])
            snprintf(buffer, sizeof(buffer), "最近阅读：%s (%d%%)", status.recent_book,
                     ui_battery_percent(status.reading_percent));
        else
            snprintf(buffer, sizeof(buffer), "最近阅读：暂无阅读记录");
        text_update(view->recent, buffer);
    }
    snprintf(buffer, sizeof(buffer), "天气：%s", status.weather[0] ? status.weather : "暂无天气数据");
    text_update(view->weather, buffer);
}

void launcher_set_status(const launcher_status_t *new_status)
{
    status = *new_status;
    status.time_text[sizeof(status.time_text) - 1] = '\0';
    status.clock_text[sizeof(status.clock_text) - 1] = '\0';
    status.recent_book[sizeof(status.recent_book) - 1] = '\0';
    status.weather[sizeof(status.weather) - 1] = '\0';
    if (initialized && current && current->foreground) status_render(&current->view);
    dynamic_headers_refresh();
}

static void header_create(lv_obj_t *screen, status_view_t *view, bool show_battery)
{
    const int status_height = 74;
    const lv_area_t status_area = {0, 0, WIDTH - 1, status_height - 1};
    epd_refresh_status_area(screen, &status_area);
    const lv_font_t *font = ui_font_body();
    int text_y = (status_height - font->line_height) / 2;
    view->time = label_create(screen, "", MARGIN, text_y, 380, font);
    view->bluetooth = icon_create(screen, &ui_icon_bt_disconnected, 0, 22);
    view->wifi = icon_create(screen, &ui_icon_wifi_disconnected, 0, 22);
    if (show_battery)
    {
        view->battery = icon_create(screen, &ui_icon_battery_empty, WIDTH - MARGIN - 128, 18);
        view->percent = label_create(screen, "", WIDTH - MARGIN - 80, text_y, 80, font);
        lv_obj_set_style_text_align(view->percent, LV_TEXT_ALIGN_RIGHT, 0);
    }
    lv_obj_t *line = panel_create(screen, MARGIN, status_height, CONTENT_WIDTH, 1);
    lv_obj_set_style_radius(line, 0, 0);
}

static void app_header_deleted(lv_event_t *event)
{
    app_header_t *header = lv_event_get_user_data(event);
    app_header_t **link = &app_headers;
    while (*link && *link != header) link = &(*link)->next;
    if (*link) *link = header->next;
    lv_free(header);
}

void epd_app_header(lv_obj_t *screen)
{
    app_header_t *header = lv_malloc_zeroed(sizeof(*header));
    if (!header) return;
    header->screen = screen;
    header->next = app_headers;
    app_headers = header;
    header_create(screen, &header->view, true);
    lv_obj_add_event_cb(screen, app_header_deleted, LV_EVENT_DELETE, header);
    status_render(&header->view);
}

void epd_app_header_refresh(lv_obj_t *screen)
{
    for (app_header_t *header = app_headers; header; header = header->next)
        if (header->screen == screen) status_render(&header->view);
}

static void dynamic_headers_refresh(void)
{
    epd_app_header_refresh(lv_screen_active());
}

lv_obj_t *centered_label(lv_obj_t *parent, const char *text, int x, int y,
                                int width, const lv_font_t *font)
{
    lv_obj_t *label = label_create(parent, text, x, y, width, font);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    return label;
}

void object_visible(lv_obj_t *object, bool visible)
{
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN) == !visible) return;
    if (visible) lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}

bool focus_eligible(lv_obj_t *object)
{
    return epd_input_focusable(object);
}

void focus_restore(unsigned index)
{
    if (index < current->focus_count && focus_eligible(current->focus_items[index]))
    {
        lv_group_focus_obj(current->focus_items[index]);
        return;
    }
    for (unsigned i = 0; i < current->focus_count; ++i)
        if (focus_eligible(current->focus_items[i]))
        {
            lv_group_focus_obj(current->focus_items[i]);
            return;
        }
}

void popup_close(void)
{
    if (!current->popup) return;
    lv_obj_delete(current->popup);
    current->popup = NULL;
    current->popup_text = current->popup_button = NULL;
    while (current->focus_count > current->popup_focus_start)
        current->focus_items[--current->focus_count] = NULL;
    for (unsigned i = 0; i < current->focus_count; ++i) lv_group_add_obj(current->group, current->focus_items[i]);
    if (focus_eligible(current->popup_previous_focus)) lv_group_focus_obj(current->popup_previous_focus);
    else focus_restore(0);
    current->popup_previous_focus = NULL;
}

void popup_open(const char *text, const char *detail)
{
    if (current->popup) return;
    current->popup_focus_start = current->focus_count;
    current->popup_previous_focus = lv_group_get_focused(current->group);
    lv_group_remove_all_objs(current->group);
    current->popup = panel_create(lv_screen_active(), 0, 0, WIDTH, HEIGHT);
    lv_obj_add_flag(current->popup, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(current->popup, 0, 0);
    lv_obj_set_style_border_width(current->popup, 0, 0);
    lv_obj_set_style_bg_color(current->popup, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(current->popup, LV_OPA_TRANSP, 0);
    lv_obj_t *dialog = panel_create(current->popup, 54, 438, 576, 282);
    lv_obj_set_style_border_color(dialog, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_width(dialog, 2, 0);
    current->popup_text = centered_label(dialog, text, 24, detail ? 44 : 74, 528, ui_font_body());
    if (detail) centered_label(dialog, detail, 24, 102, 528, ui_font_small());
    current->popup_button = button_create(dialog, 108, 184, 360, 64, "确定", POPUP_CLOSE);
    lv_group_focus_obj(current->popup_button);
}

void popup_confirm(const char *text, const char *detail, int target)
{
    popup_open(text, detail);
    lv_obj_t *button = current->popup_button;
    lv_obj_set_x(button, 306);
    lv_obj_set_width(button, 222);
    lv_obj_t *label = lv_obj_get_child(button, 0);
    lv_obj_set_width(label, 198);
    lv_obj_center(label);
    lv_obj_remove_event_cb(button, clicked);
    lv_obj_add_event_cb(button, clicked, LV_EVENT_CLICKED, (void *)(intptr_t)target);
    lv_obj_t *cancel = button_create(lv_obj_get_parent(button), 48, 184, 222, 64, "取消", POPUP_CLOSE);
    lv_group_focus_obj(cancel);
}

static void page_create(void)
{
    lv_obj_t *screen = current->screen;
    ui_page_id_t id = current->id;
    epd_obj_init(screen);
    lv_obj_set_size(screen, WIDTH, HEIGHT);
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(screen, ui_font_body(), 0);
    header_create(screen, &current->view, id != UI_PAGE_LOCK);
    ui_page_operations(id)->create(screen);
    if (id != UI_PAGE_HOME && id != UI_PAGE_LOCK)
        icon_button_create(screen, MARGIN,
                           BACK_BUTTON_Y,
                           &ui_icon_back, NAV_BACK);
}

static void page_lifecycle(ui_page_id_t id, gui_app_msg_type_t message, void *memory)
{
    page_context_t *context = memory;
    page_context_t *previous = current;
    current = context;
    switch (message)
    {
    case GUI_APP_MSG_ONSTART:
        context->id = id;
        context->screen = lv_screen_active();
        context->saved_focus = saved_focus[id];
        context->group = epd_app_input_group(context->screen);
        LV_ASSERT_MALLOC(context->group);
        epd_input_set_ops(context->group, &page_input_ops);
        page_create();
        focus_restore(context->saved_focus);
        current = previous;
        break;
    case GUI_APP_MSG_ONRESUME:
        context->foreground = true;
        status_render(&current->view);
        if (ui_page_operations(id)->resume) ui_page_operations(id)->resume();
        if (context->focus_count && !lv_group_get_focused(context->group))
            lv_group_focus_obj(context->focus_items[context->saved_focus < context->focus_count ?
                                                   context->saved_focus : 0]);
        break;
    case GUI_APP_MSG_ONPAUSE:
        context->saved_focus = launcher_focus_index();
        saved_focus[id] = context->saved_focus;
        if (ui_page_operations(id)->pause) ui_page_operations(id)->pause();
        context->foreground = false;
        /* Page polling is foreground-only; independent services keep running. */
        current = NULL;
        break;
    case GUI_APP_MSG_ONSTOP:
        if (ui_page_operations(id)->stop) ui_page_operations(id)->stop();
        context->foreground = false;
        /* Screen deletion releases the input group, dialogs and framework context. */
        current = previous == context ? NULL : previous;
        break;
    default:
        current = previous;
        break;
    }
}

bool launcher_init(void)
{
    if (initialized) return true;
    if (!ui_font_init()) return false;
    /* Use static, flat styles without the default theme's interaction effects. */
    lv_display_set_theme(NULL, NULL);
    lv_style_init(&epd_style);
    lv_style_set_transition(&epd_style, NULL);
    lv_style_set_anim_duration(&epd_style, 0);
    lv_style_set_bg_grad_dir(&epd_style, LV_GRAD_DIR_NONE);
    lv_style_set_bg_grad(&epd_style, NULL);
    lv_style_set_shadow_width(&epd_style, 0);
    initialized = ui_navigation_init(page_lifecycle, sizeof(page_context_t));
    return initialized;
}

ui_page_id_t launcher_current_page(void)
{
    return current ? current->id : UI_PAGE_COUNT;
}

void launcher_open(ui_page_id_t page)
{
    if (!current) return;
    if (initialized && pending_page == NAV_IDLE && page >= UI_PAGE_HOME && page < UI_PAGE_COUNT)
        pending_page = page;
}

static bool page_input_ready(uint32_t key)
{
    (void)key;
    return initialized && current && current->foreground && pending_page == NAV_IDLE;
}

static bool page_input_move(const epd_input_event_t *events, unsigned count)
{
    (void)events;
    (void)count;
    return launcher_current_page() == UI_PAGE_LOCK;
}

static bool page_input_key(uint32_t key)
{
    if (launcher_current_page() == UI_PAGE_LOCK)
    {
        if (key == LV_KEY_ENTER) pending_page = NAV_BACK;
        return true;
    }
    if (key == LV_KEY_ESC)
    {
        if (current->popup) pending_page = POPUP_CLOSE;
        else if (launcher_current_page() != UI_PAGE_HOME) pending_page = NAV_BACK;
        return true;
    }
    return false;
}

void launcher_process(void)
{
    if (!initialized) return;
    app_service_process();
    app_installer_process();
    app_catalog_process();
    home_process();
    if (!current || !current->foreground) return;
    settings_process();
    if (pending_page == NAV_IDLE) return;
    int target = pending_page;
    pending_page = NAV_IDLE;
    const ui_page_ops_t *ops = ui_page_operations(launcher_current_page());
    if (ops->action && ops->action(&target)) return;
    if (target == POPUP_CLOSE)
    {
        popup_close();
        return;
    }
    bool navigated = target == NAV_BACK ? ui_navigation_back() :
                     ui_navigation_open((ui_page_id_t)target);
    if (!navigated)
    {
        if (current && current->foreground)
            popup_open(target == NAV_BACK ? "返回失败，请重试" : "页面打开失败，请重试", NULL);
    }
}

bool launcher_storage_process(void)
{
    static uint32_t observed_revision;
    static uint32_t releasing_id;
    static bool release_acknowledged;
    app_card_process();
    uint32_t request_id = storage_release_request_id();
    if (request_id)
    {
        if (!lv_refreshing_done()) return false;
        if (releasing_id != request_id)
        {
            releasing_id = request_id;
            release_acknowledged = false;
            pending_page = NAV_IDLE;
            if (current && current->id == UI_PAGE_HOME) home_pause();
            app_catalog_changed();
        }
        if (!release_acknowledged)
        {
            if (!app_catalog_release_storage()) return storage_release_timed_out();
            release_acknowledged = storage_release_complete(releasing_id) == RT_EOK;
        }
        return false;
    }
    releasing_id = 0;
    release_acknowledged = false;
    if (storage_changing()) return false;
    uint32_t next_revision = storage_revision();
    if (next_revision != observed_revision)
    {
        observed_revision = next_revision;
        app_installer_storage_changed();
        app_catalog_changed();
        if (current && current->foreground)
        {
            switch (current->id)
            {
            case UI_PAGE_FILES: files_storage_changed(); break;
            case UI_PAGE_STORAGE: storage_page_resume(); break;
            case UI_PAGE_APP_INSTALL:
            case UI_PAGE_APP_UNINSTALL:
            case UI_PAGE_APP_SETTINGS: app_list_storage_changed(); break;
            default: break;
            }
            if (!focus_eligible(lv_group_get_focused(current->group))) focus_restore(0);
        }
    }
    return true;
}
