#include "app_settings_registry.h"
#include <string.h>
#include <stddef.h>

static const app_setting_entry_t *entries[24];
static unsigned count;

bool app_settings_register(const app_setting_entry_t *entry)
{
    if (!entry || !entry->id || !entry->title || entry->page > UI_PAGE_COUNT) return false;
    for (unsigned i = 0; i < count; ++i)
        if (!strcmp(entries[i]->id, entry->id)) { entries[i] = entry; return true; }
    if (count == sizeof(entries) / sizeof(entries[0])) return false;
    entries[count++] = entry;
    return true;
}

const app_setting_entry_t *app_settings_get(unsigned index) { return index < count ? entries[index] : NULL; }
