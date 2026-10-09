#include "ui_internal.h"


static int app_main(intent_t intent)
{
    (void)intent;
    return ui_navigation_start("Main", UI_PAGE_HOME);
}
BUILTIN_APP_EXPORT("EPD Reader", NULL, "Main", app_main, 1);

#include "platform/app_catalog.h"
#include "storage_file.h"

#define HOME_APP_BASE (-400)
#define HOME_PREVIOUS (-410)
#define HOME_NEXT (-411)
#define HOME_QUICK_HEIGHT 224
#define HOME_DOT_SIZE 12
#define HOME_DOT_PITCH 26

static unsigned home_first;
static uint32_t home_revision;

static void home_update(bool reset_focus)
{
    unsigned count = app_catalog_count();
    unsigned pages = count / HOME_PAGE_SIZE + (count % HOME_PAGE_SIZE != 0);
    while (lv_obj_get_child_count(current->home_pager) > pages)
        lv_obj_delete(lv_obj_get_child(current->home_pager, -1));
    while (lv_obj_get_child_count(current->home_pager) < pages)
    {
        lv_obj_t *dot = panel_create(current->home_pager, 0, 14, HOME_DOT_SIZE, HOME_DOT_SIZE);
        if (!dot) break;
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_color(dot, lv_color_black(), 0);
        lv_obj_set_style_bg_color(dot, lv_color_black(), LV_STATE_CHECKED);
    }
    if (home_first >= count)
        home_first = pages ? (pages - 1) * HOME_PAGE_SIZE : 0;
    for (unsigned i = 0; i < HOME_PAGE_SIZE; ++i)
    {
        const epd_app_entry_t *entry = app_catalog_get(home_first + i);
        object_visible(current->home_tiles[i], entry != NULL);
        if (!entry) { epd_app_image_release(current->home_icons[i]); continue; }
        char path[256];
        const char *file = NULL;
        const lv_image_dsc_t *fallback = &ui_icon_album;
        if (lv_image_src_get_type(entry->icon) == LV_IMAGE_SRC_FILE)
            file = entry->icon;
        else
            fallback = entry->icon;
        if (entry->icon_file[0] && storage_app_path(path, sizeof(path), entry->id,
                                                   STORAGE_APP_CODE, entry->icon_file)) file = path;
        epd_app_image_set(current->home_icons[i], file, fallback);
        text_update(current->home_labels[i], entry->name);
    }
    object_visible(current->home_pager, pages > 1);
    for (unsigned i = 0; i < lv_obj_get_child_count(current->home_pager); ++i)
    {
        lv_obj_t *dot = lv_obj_get_child(current->home_pager, i);
        object_visible(dot, i < pages);
        if (i >= pages) continue;
        unsigned span = CONTENT_WIDTH - HOME_DOT_SIZE;
        if (pages - 1 <= span / HOME_DOT_PITCH) span = (pages - 1) * HOME_DOT_PITCH;
        unsigned offset = pages > 1 ? (uint64_t)i * span / (pages - 1) : 0;
        lv_obj_set_x(dot, (CONTENT_WIDTH - span - HOME_DOT_SIZE) / 2 + offset);
        if (i == home_first / HOME_PAGE_SIZE) lv_obj_add_state(dot, LV_STATE_CHECKED);
        else lv_obj_remove_state(dot, LV_STATE_CHECKED);
    }
    if (reset_focus || !focus_eligible(lv_group_get_focused(current->group))) focus_restore(0);
}

static void home_gesture(lv_event_t *event)
{
    if (!current || !current->foreground || current->id != UI_PAGE_HOME ||
        lv_event_get_current_target_obj(event) != current->screen || current->popup) return;
    lv_indev_t *indev = lv_event_get_indev(event);
    if (!indev) return;
    lv_dir_t direction = lv_indev_get_gesture_dir(indev);
    /* Consume this touch sequence so releasing a swipe cannot open a tile. */
    lv_indev_wait_release(indev);
    if (pending_page != NAV_IDLE) return;
    if (direction == LV_DIR_LEFT && home_first + HOME_PAGE_SIZE < app_catalog_count())
        pending_page = HOME_NEXT;
    else if (direction == LV_DIR_RIGHT && home_first)
        pending_page = HOME_PREVIOUS;
}

void home_resume(void)
{
    home_revision = app_catalog_revision();
    app_catalog_refresh();
    home_update(false);
}

void home_pause(void)
{
    for (unsigned i = 0; i < HOME_PAGE_SIZE; ++i)
        epd_app_image_release(current->home_icons[i]);
}

void home_process(void)
{
    if (current && current->foreground && current->id == UI_PAGE_HOME && !current->popup &&
        lv_refreshing_done() && home_revision != app_catalog_revision()) home_resume();
}

bool home_action(int target)
{
    if (target == HOME_PREVIOUS || target == HOME_NEXT)
    {
        if (target == HOME_NEXT)
        {
            if (home_first + HOME_PAGE_SIZE >= app_catalog_count()) return true;
            home_first += HOME_PAGE_SIZE;
        }
        else
        {
            if (!home_first) return true;
            home_first -= HOME_PAGE_SIZE;
        }
        home_update(true);
        return true;
    }
    if (target < HOME_APP_BASE || target >= HOME_APP_BASE + HOME_PAGE_SIZE) return false;
    const epd_app_entry_t *entry = app_catalog_get(home_first + target - HOME_APP_BASE);
    if (entry)
    {
        char id[GUI_APP_ID_MAX_LEN];
        snprintf(id, sizeof(id), "%s", entry->id);
        if (!ui_navigation_run(id) && current && current->foreground)
            popup_open("应用打开失败，请重试", NULL);
    }
    return true;
}

void home_create(lv_obj_t *screen)
{
    app_catalog_refresh();
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(screen, home_gesture, LV_EVENT_GESTURE, NULL);
    for (unsigned i = 0; i < HOME_PAGE_SIZE; ++i)
    {
        lv_obj_t *tile = button_create(screen, MARGIN + (i % 2) * 322,
                100 + (i / 2) * 212, 298, 190, NULL, HOME_APP_BASE + i);
        /* Touch focus is committed by clicked(), not at the start of a swipe. */
        lv_obj_remove_flag(tile, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        current->home_tiles[i] = tile;
        current->home_icons[i] = icon_create(tile, &ui_icon_album, 108, 28);
        current->home_labels[i] = label_create(tile, "", 12, 134, 272, ui_font_body());
        lv_obj_set_style_text_align(current->home_labels[i], LV_TEXT_ALIGN_CENTER, 0);
    }
    current->home_pager = lv_obj_create(screen);
    epd_obj_init(current->home_pager);
    lv_obj_remove_flag(current->home_pager, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(current->home_pager, MARGIN, 940);
    lv_obj_set_size(current->home_pager, CONTENT_WIDTH, 40);
    home_update(false);
    lv_obj_t *quick = panel_create(screen, MARGIN, HEIGHT - HOME_QUICK_HEIGHT,
                                  CONTENT_WIDTH, HOME_QUICK_HEIGHT);
    lv_obj_remove_flag(quick, LV_OBJ_FLAG_CLICKABLE);
    icon_create(quick, &ui_icon_quick, 22, 24);
    label_create(quick, "快捷信息", 66, 20, 400, ui_font_title());
    current->view.recent = label_create(quick, "", 22, 88, CONTENT_WIDTH - 46, ui_font_body());
    current->view.weather = label_create(quick, "", 22, 146, CONTENT_WIDTH - 46, ui_font_body());
}

void lock_create(lv_obj_t *screen)
{
    lv_obj_t *label = label_create(screen, "SiFli EPD DEMO", MARGIN, 290,
                                    CONTENT_WIDTH, ui_font_body());
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    icon_create(screen, &ui_icon_bookshelf, (WIDTH - 80) / 2, 388);
    label = label_create(screen, "Welcome", MARGIN, 512, CONTENT_WIDTH, ui_font_title());
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *unlock = icon_button_create(screen, (WIDTH - ICON_BUTTON_SIZE) / 2, 646,
                                         &ui_icon_key2, NAV_BACK);
    lv_obj_set_style_outline_width(unlock, 0, LV_STATE_FOCUSED);
}
