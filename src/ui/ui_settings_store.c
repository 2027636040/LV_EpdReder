#include "ui_settings_store.h"
#include "storage_file.h"
#include <string.h>

#define SETTINGS_PATH STORAGE_FLASH_ROOT "/system/settings.bin"
#define SETTINGS_MAGIC 0x45505332u
#define SETTINGS_VERSION 1u

static bool saved_valid;
static uint8_t last_saved[UI_SETTING_COUNT];

bool ui_settings_store_load(uint8_t values[UI_SETTING_COUNT])
{
    uint8_t candidate[UI_SETTING_COUNT];
    saved_valid = false;
    if (!storage_record_load(SETTINGS_PATH, SETTINGS_MAGIC, SETTINGS_VERSION,
                             candidate, sizeof(candidate))) return false;
    for (unsigned i = 0; i < UI_SETTING_COUNT; ++i)
        if (candidate[i] >= ui_settings_item((ui_setting_id_t)i)->option_count) return false;
    memcpy(values, candidate, sizeof(candidate));
    memcpy(last_saved, candidate, sizeof(candidate));
    saved_valid = true;
    return true;
}

bool ui_settings_store_save(const uint8_t values[UI_SETTING_COUNT])
{
    if (saved_valid && !memcmp(values, last_saved, sizeof(last_saved))) return true;
    if (!storage_record_save(SETTINGS_PATH, SETTINGS_MAGIC, SETTINGS_VERSION,
                             values, sizeof(last_saved))) return false;
    memcpy(last_saved, values, sizeof(last_saved));
    saved_valid = true;
    return true;
}
