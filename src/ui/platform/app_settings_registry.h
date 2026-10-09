#ifndef EPD_APP_SETTINGS_REGISTRY_H
#define EPD_APP_SETTINGS_REGISTRY_H
#include "ui_model.h"

typedef struct
{
    const char *id;
    const char *title;
    ui_page_id_t page;
} app_setting_entry_t;

/* Entries and their strings live for the lifetime of the firmware. */
bool app_settings_register(const app_setting_entry_t *entry);
const app_setting_entry_t *app_settings_get(unsigned index);
#endif
