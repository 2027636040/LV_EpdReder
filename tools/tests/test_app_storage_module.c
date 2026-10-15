#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RT_NAME_MAX 16
#define STORAGE_SD_ROOT "/sdcard"
typedef int rt_base_t;
typedef enum { STORAGE_FLASH, STORAGE_SD } storage_volume_t;
struct rt_dlmodule { char name[RT_NAME_MAX]; unsigned refs; };
static struct rt_dlmodule loaded[8];
static bool card_available = true, fail_load, remove_during_load;
static uint32_t card_session = 1;
static unsigned allocations;
static int fail_allocation = -1;

static void *rt_calloc(size_t count, size_t size)
{
    if (fail_allocation == 0) return NULL;
    if (fail_allocation > 0) --fail_allocation;
    void *memory = calloc(count, size);
    if (memory) ++allocations;
    return memory;
}
static void rt_free(void *memory)
{ if (memory) { assert(allocations); --allocations; free(memory); } }
static rt_base_t rt_hw_interrupt_disable(void) { return 0; }
static void rt_hw_interrupt_enable(rt_base_t level) { (void)level; }
static const char *storage_path_root(const char *path)
{ return !strncmp(path, "/sdcard/", 8) ? STORAGE_SD_ROOT : "/flash"; }
static uint32_t storage_card_session(void) { return card_session; }
static bool storage_apps_enabled(storage_volume_t volume)
{ return volume == STORAGE_FLASH || card_available; }
static bool storage_session_valid(storage_volume_t volume, uint32_t session)
{ return volume == STORAGE_FLASH || (card_available && card_session == session); }
static struct rt_dlmodule *dlmodule_find(const char *name)
{
    for (unsigned i = 0; i < 8; ++i)
        if (loaded[i].refs && !strcmp(loaded[i].name, name)) return &loaded[i];
    return NULL;
}
static void *dlopen(const char *path, int flags)
{
    (void)flags;
    if (fail_load) return NULL;
    const char *base = strrchr(path, '/') + 1;
    char name[RT_NAME_MAX] = {0};
    memcpy(name, base, strlen(base) - 3);
    struct rt_dlmodule *module = dlmodule_find(name);
    if (!module)
        for (unsigned i = 0; i < 8; ++i)
            if (!loaded[i].refs) { module = &loaded[i]; strcpy(module->name, name); break; }
    assert(module);
    ++module->refs;
    if (remove_during_load) card_available = false;
    return module;
}
static void dlclose(void *handle)
{ struct rt_dlmodule *module = handle; assert(module->refs); --module->refs; }

#include "module_under_test.h"

/* The installer must wait for the app's private libraries too, even after the
 * SDK has destroyed its main module and app_fwk has removed its pages. */
typedef uint32_t rt_tick_t;
typedef void *rt_list_t;
typedef struct { const char *id; } gui_runing_app_t;
static bool needs_prepare, exit_sent, prepared_resources, prepare_ok;
static const char *job_id = "books";
static rt_tick_t prepare_started, now;
static unsigned prepared, release_calls;
static void start_card_recovery(void) {}
static void gui_app_exit(const char *id) { assert(!strcmp(id, job_id)); }
static void gui_app_exec_now(void) {}
static void app_service_stop(const char *id) { assert(!strcmp(id, job_id)); }
static bool app_service_running(const char *id) { (void)id; return false; }
static gui_runing_app_t *gui_app_trav(rt_list_t **cursor) { (void)cursor; return NULL; }
static rt_tick_t rt_tick_get(void) { return now; }
static rt_tick_t rt_tick_from_millisecond(uint32_t ms) { return ms; }
static void rt_sem_release(unsigned *semaphore) { assert(semaphore == &prepared); ++release_calls; }
#include "installer_lifetime_under_test.h"

int main(void)
{
    storage_volume_t volume;
    uint32_t session;
    void *main_module = app_module_open("/sdcard/apps/books/books.so", "books");
    assert(main_module && app_module_loaded("books"));
    assert(app_module_storage("books", &volume, &session));
    assert(volume == STORAGE_SD && session == 1);
    void *pin = app_module_open("/sdcard/apps/books/books.so", "books");
    void *codec = app_module_open("/sdcard/apps/books/codecs/mobi.so", "books");
    assert(pin == main_module && codec && ((struct rt_dlmodule *)pin)->refs == 2);
    assert(!app_module_open("/flash/apps/books/books.so", "books"));
    assert(!app_module_open("/sdcard/apps/other/mobi.so", "other"));
    assert(!app_module_loaded("other"));
    app_module_close(main_module);
    assert(app_module_loaded("books") && ((struct rt_dlmodule *)pin)->refs == 1);
    app_module_close(pin);
    assert(app_module_loaded("books") && app_modules_present());
    assert(!dlmodule_find("books"));
    needs_prepare = true;
    app_installer_process();
    assert(needs_prepare && !prepare_ok && !release_calls);
    card_available = false;
    assert(app_module_storage("books", &volume, &session));
    assert(!storage_session_valid(volume, session));
    card_available = true;
    ++card_session;
    assert(!app_module_open("/sdcard/apps/books/books.so", "books"));
    app_module_close(codec);
    assert(!app_modules_present() && !allocations);
    app_installer_process();
    assert(!needs_prepare && prepare_ok && release_calls == 1);

    for (int i = 0; i < 2; ++i)
    {
        fail_allocation = i;
        assert(!app_module_open("/sdcard/apps/books/books.so", "books"));
        assert(!app_modules_present() && !allocations);
    }
    fail_allocation = -1;
    fail_load = true;
    assert(!app_module_open("/sdcard/apps/books/books.so", "books"));
    assert(!app_modules_present() && !allocations);
    fail_load = false;
    remove_during_load = true;
    assert(!app_module_open("/sdcard/apps/books/books.so", "books"));
    assert(!app_modules_present() && !allocations && !dlmodule_find("books"));
    remove_during_load = false;
    main_module = app_module_open("/flash/apps/books/books.so", "books");
    assert(main_module && app_module_storage("books", &volume, &session));
    assert(storage_session_valid(volume, session));
    needs_prepare = true;
    now = 35000;
    app_installer_process();
    assert(!needs_prepare && !prepare_ok && release_calls == 2);
    assert(app_module_loaded("books")); /* Timeout never forces an unload. */
    app_module_close(main_module);
    assert(!app_modules_present() && !allocations);
    puts("module: native references, private-library install barrier, storage binding and failure cleanup passed");
    return 0;
}
