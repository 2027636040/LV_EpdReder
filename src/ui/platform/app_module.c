/* SPDX-License-Identifier: Apache-2.0 */
#include "app_module.h"
#include <dlmodule.h>
#include <dlfcn.h>
#include <rthw.h>
#include <string.h>

typedef struct app_module
{
    struct app_module *next;
    void *module;
    char name[RT_NAME_MAX];
} app_module_t;

typedef struct app_instance
{
    struct app_instance *next;
    char id[RT_NAME_MAX];
    storage_volume_t volume;
    uint32_t session;
    app_module_t *modules;
} app_instance_t;

static app_instance_t *instances;

bool app_module_storage(const char *id, storage_volume_t *volume, uint32_t *session)
{
    bool found = false;
    rt_base_t level = rt_hw_interrupt_disable();
    for (app_instance_t *instance = instances; instance; instance = instance->next)
        if (!strcmp(instance->id, id))
        {
            *volume = instance->volume;
            *session = instance->session;
            found = true;
            break;
        }
    rt_hw_interrupt_enable(level);
    return found;
}

bool app_modules_present(void)
{
    return instances != NULL;
}

bool app_module_loaded(const char *id)
{
    bool found = false;
    rt_base_t level = rt_hw_interrupt_disable();
    for (app_instance_t *instance = instances; instance; instance = instance->next)
        if (!strcmp(instance->id, id))
        {
            found = true;
            break;
        }
    rt_hw_interrupt_enable(level);
    return found;
}

static void instance_release_empty(app_instance_t *instance)
{
    if (instance->modules) return;
    rt_base_t level = rt_hw_interrupt_disable();
    app_instance_t **link = &instances;
    while (*link && *link != instance) link = &(*link)->next;
    if (*link) *link = instance->next;
    rt_hw_interrupt_enable(level);
    rt_free(instance);
}

void *app_module_open(const char *path, const char *id)
{
    if (!path || !id || !*id || strlen(id) >= RT_NAME_MAX) return NULL;
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    size_t length = strlen(name);
    if (length <= 3 || strcmp(name + length - 3, ".so") || length - 3 >= RT_NAME_MAX) return NULL;
    char module_name[RT_NAME_MAX];
    memcpy(module_name, name, length - 3);
    module_name[length - 3] = 0;
    app_instance_t *instance = instances;
    while (instance && strcmp(instance->id, id)) instance = instance->next;
    bool fresh = instance == NULL;
    storage_volume_t volume = !strcmp(storage_path_root(path), STORAGE_SD_ROOT) ? STORAGE_SD : STORAGE_FLASH;
    uint32_t session = storage_card_session();
    if (!storage_apps_enabled(volume) || (!fresh &&
        (instance->volume != volume || !storage_session_valid(volume, instance->session)))) return NULL;
    if (fresh)
    {
        instance = rt_calloc(1, sizeof(*instance));
        if (!instance) return NULL;
        strcpy(instance->id, id);
        instance->volume = volume;
        instance->session = session;
        rt_base_t level = rt_hw_interrupt_disable();
        instance->next = instances;
        instances = instance;
        rt_hw_interrupt_enable(level);
    }
    app_module_t *record = instance->modules;
    while (record && strcmp(record->name, module_name)) record = record->next;
    /* The SDK keys loaded modules by basename, not by installation directory. */
    struct rt_dlmodule *existing = dlmodule_find(module_name);
    if (existing && (!record || record->module != existing))
    {
        instance_release_empty(instance);
        return NULL;
    }
    bool new_record = record == NULL;
    if (new_record) record = rt_calloc(1, sizeof(*record));
    if (!record) { instance_release_empty(instance); return NULL; }
    struct rt_dlmodule *module = dlopen(path, 0);
    /* Removal may race a successful final read. Never run its entry afterward. */
    if (module && !storage_session_valid(volume, session))
    { dlclose(module); module = NULL; }
    rt_base_t level = rt_hw_interrupt_disable();
    if (module)
    {
        record->module = module;
        strcpy(record->name, module_name);
        if (new_record)
        {
            record->next = instance->modules;
            instance->modules = record;
        }
    }
    rt_hw_interrupt_enable(level);
    if (!module)
    {
        if (new_record) rt_free(record);
        instance_release_empty(instance);
    }
    return module;
}

void app_module_close(void *module)
{
    if (!module) return;
    app_instance_t *instance = NULL;
    app_module_t *record = NULL;
    for (app_instance_t *item = instances; item && !record; item = item->next)
        for (app_module_t *m = item->modules; m; m = m->next)
            if (m->module == module)
            { instance = item; record = m; break; }
    dlclose(module);
    /* Background services may still hold nref. Only retire a destroyed module. */
    if (record && dlmodule_find(record->name) != module)
    {
        rt_base_t level = rt_hw_interrupt_disable();
        app_module_t **link = &instance->modules;
        while (*link != record) link = &(*link)->next;
        *link = record->next;
        rt_hw_interrupt_enable(level);
        rt_free(record);
        instance_release_empty(instance);
    }
}
