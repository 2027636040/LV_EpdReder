#ifndef UI_INTERNAL_H
#define UI_INTERNAL_H

#include "launcher.h"
#include "ui_font.h"
#include "icons/ui_icons.h"
#include "bf0_hal.h"
#include "ui_settings.h"
#include "ui_app.h"
#include "ui_navigation.h"
#include "platform/app_image.h"
#include "boards/epd_e0470a03_57x/epd_waveform.h"
#include <stdio.h>
#include <string.h>

#define WIDTH 684
#define HEIGHT 1216
#define MARGIN 32
#define CONTENT_WIDTH (WIDTH - 2 * MARGIN)
#define PAGE_TITLE_X (MARGIN + 72)
#define PAGE_TITLE_Y 103
#define PAGE_CONTENT_Y 174
#define ICON_BUTTON_SIZE 56
#define BACK_BUTTON_Y 94
#define FOCUS_CAPACITY 24
#define HOME_PAGE_SIZE 8
#define NAV_BACK (-2)
#define NAV_IDLE (-1)
#define POPUP_CLOSE (-5)
#define SETTING_CHANGE_BASE (-192)

extern bool lv_refreshing_done(void);

typedef struct
{
    lv_obj_t *time, *wifi, *bluetooth, *battery, *percent;
    lv_obj_t *recent, *weather;
} status_view_t;

typedef struct
{
    lv_obj_t *value;
    lv_obj_t *track;
    lv_obj_t *knob;
    bool rendered, enabled;
} setting_view_t;

typedef struct
{
    ui_page_id_t id;
    bool foreground;
    unsigned saved_focus;
    void *feature;
    lv_obj_t *screen;
    lv_group_t *group;
    lv_obj_t *focus_items[FOCUS_CAPACITY];
    unsigned focus_count;
    status_view_t view;
    lv_obj_t *home_tiles[HOME_PAGE_SIZE], *home_icons[HOME_PAGE_SIZE], *home_labels[HOME_PAGE_SIZE];
    lv_obj_t *home_pager;
    lv_obj_t *popup;
    lv_obj_t *popup_text, *popup_button;
    lv_obj_t *popup_previous_focus;
    unsigned popup_focus_start;
    setting_view_t setting_views[UI_SETTING_COUNT];
} page_context_t;

extern page_context_t *current;
typedef struct
{
    void (*create)(lv_obj_t *screen);
    void (*resume)(void);
    void (*pause)(void);
    void (*stop)(void);
    bool (*action)(int *target);
} ui_page_ops_t;

const ui_page_ops_t *ui_page_operations(ui_page_id_t page);
bool settings_action(int *target);
void settings_resume(void);
void settings_process(void);
extern launcher_status_t status;
extern int pending_page;
extern const char *const titles[UI_PAGE_COUNT];

void epd_obj_init(lv_obj_t *obj);
void text_update(lv_obj_t *label, const char *text);
lv_obj_t *label_create(lv_obj_t *parent, const char *text, int x, int y,
                              int width, const lv_font_t *font);
lv_obj_t *icon_create(lv_obj_t *parent, const lv_image_dsc_t *src, int x, int y);
lv_obj_t *panel_create(lv_obj_t *parent, int x, int y, int width, int height);
lv_obj_t *button_create(lv_obj_t *parent, int x, int y, int width, int height,
                               const char *text, int target);
lv_obj_t *icon_button_create(lv_obj_t *parent, int x, int y,
                                    const lv_image_dsc_t *src, int target);
lv_obj_t *page_title_create(lv_obj_t *screen, const char *text, const lv_font_t *font);
lv_obj_t *centered_label(lv_obj_t *parent, const char *text, int x, int y,
                                int width, const lv_font_t *font);
void object_visible(lv_obj_t *object, bool visible);
bool focus_eligible(lv_obj_t *object);
void focus_restore(unsigned index);
void popup_close(void);
void popup_open(const char *text, const char *detail);
void popup_confirm(const char *text, const char *detail, int target);
void image_update(lv_obj_t *icon, const lv_image_dsc_t *src);
void home_create(lv_obj_t *screen);
void home_resume(void);
void home_pause(void);
void home_process(void);
bool home_action(int target);
void lock_create(lv_obj_t *screen);
void settings_create(lv_obj_t *screen);
void text_settings_create(lv_obj_t *screen);
void display_settings_create(lv_obj_t *screen);
void network_settings_create(lv_obj_t *screen);
void wifi_settings_create(lv_obj_t *screen);
void about_create(lv_obj_t *screen);
void storage_page_create(lv_obj_t *screen);
void storage_page_resume(void);
void files_create(lv_obj_t *screen);
void files_resume(void);
void files_storage_changed(void);
void files_stop(void);
bool files_action(int *target);
void app_management_create(lv_obj_t *screen);
void app_list_create(lv_obj_t *screen);
void app_list_resume(void);
void app_list_stop(void);
bool app_list_action(int *target);
void app_list_process(void);
void app_list_storage_changed(void);
void memory_page_create(lv_obj_t *screen);
void memory_page_resume(void);
void memory_page_stop(void);
bool memory_page_action(int *target);

#endif
