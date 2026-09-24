#ifndef UI_SETTINGS_STORE_H
#define UI_SETTINGS_STORE_H
#include "ui_settings.h"
#include <stdint.h>

/* Application-thread-only storage; two independent NOR sectors. */
bool ui_settings_store_load(uint8_t values[UI_SETTING_COUNT]);
bool ui_settings_store_save(const uint8_t values[UI_SETTING_COUNT]);
#endif
