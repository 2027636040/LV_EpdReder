#include "ui_internal.h"

static bool home_dispatch(int *target)
{
    return home_action(*target);
}


static const ui_page_ops_t pages[UI_PAGE_COUNT] = {
    [UI_PAGE_HOME] = {home_create, home_resume, home_pause, NULL, home_dispatch},
    [UI_PAGE_SETTINGS] = {settings_create, settings_resume, NULL, NULL, settings_action},
    [UI_PAGE_TEXT_SETTINGS] = {text_settings_create, settings_resume, NULL, NULL, settings_action},
    [UI_PAGE_LOCK] = {lock_create, NULL, NULL, NULL, NULL},
    [UI_PAGE_FILES] = {files_create, files_resume, NULL, files_stop, files_action},
    [UI_PAGE_ABOUT] = {about_create, NULL, NULL, NULL, NULL},
    [UI_PAGE_DISPLAY_SETTINGS] = {display_settings_create, settings_resume, NULL, NULL, settings_action},
    [UI_PAGE_NETWORK_SETTINGS] = {network_settings_create, settings_resume, NULL, NULL, settings_action},
    [UI_PAGE_WIFI_SETTINGS] = {wifi_settings_create, settings_resume, NULL, NULL, settings_action},
    [UI_PAGE_STORAGE] = {storage_page_create, storage_page_resume, NULL, files_stop, files_action},
    [UI_PAGE_APP_MANAGEMENT] = {app_management_create, NULL, NULL, NULL, settings_action},
    [UI_PAGE_APP_INSTALL] = {app_list_create, app_list_resume, NULL, app_list_stop, app_list_action},
    [UI_PAGE_APP_UNINSTALL] = {app_list_create, app_list_resume, NULL, app_list_stop, app_list_action},
    [UI_PAGE_APP_SETTINGS] = {app_list_create, app_list_resume, NULL, app_list_stop, app_list_action},
    [UI_PAGE_MEMORY] = {memory_page_create, memory_page_resume, NULL, memory_page_stop, memory_page_action}
};

const ui_page_ops_t *ui_page_operations(ui_page_id_t page)
{
    RT_ASSERT(page < UI_PAGE_COUNT);
    return &pages[page];
}
