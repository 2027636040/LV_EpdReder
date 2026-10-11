#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform/app_recovery.h"
#include "platform/app_catalog.h"
#include "platform/app_package.h"
#include "platform/app_installer.h"
#include "platform/app_module.h"
#include "app_service.h"
#include "epd_memory.h"
#include "ui_model.h"

enum { IO_OK, IO_STAT, IO_OPEN_READ, IO_OPEN_WRITE, IO_READ, IO_FSTAT,
       IO_WRITE, IO_SYNC, IO_CLOSE, IO_RENAME };
enum { O_RDONLY = 0, O_WRONLY = 1, O_CREAT = 2, O_TRUNC = 4 };
enum { S_REG = 1, S_DIR = 2 };
#define S_ISREG(mode) ((mode) == S_REG)
#define S_ISDIR(mode) ((mode) == S_DIR)
struct stat { size_t st_size; int st_mode; };
typedef struct { char path[160]; uint8_t data[128]; size_t size; bool exists; } file_t;
typedef struct { file_t *file; size_t position; bool writing, active; } fd_t;
static file_t files[8];
static fd_t descriptors[8];
static int io_failure, locks, irq_depth;
static unsigned writes;
static bool flash_available = true;
static char shell_output[4096];

rt_base_t rt_hw_interrupt_disable(void) { return irq_depth++; }
void rt_hw_interrupt_enable(rt_base_t level) { assert(irq_depth == level + 1); irq_depth = level; }
void rt_set_errno(rt_err_t error) { errno = error; }
void rt_kprintf(const char *format, ...)
{
    size_t used = strlen(shell_output);
    va_list args;
    va_start(args, format);
    vsnprintf(shell_output + used, sizeof(shell_output) - used, format, args);
    va_end(args);
}
void storage_lock(void) { assert(!irq_depth); ++locks; }
void storage_unlock(void) { assert(locks > 0); --locks; }
bool storage_available(storage_volume_t volume) { return volume == STORAGE_FLASH ? flash_available : true; }
bool storage_path_available(const char *path) { (void)path; return flash_available; }
const char *storage_path_root(const char *path)
{ return !strncmp(path, STORAGE_SD_ROOT, strlen(STORAGE_SD_ROOT)) ? STORAGE_SD_ROOT : STORAGE_FLASH_ROOT; }
bool app_module_storage(const char *id, storage_volume_t *volume, uint32_t *session)
{ (void)id; (void)volume; (void)session; return false; }
bool storage_session_valid(storage_volume_t volume, uint32_t session)
{ (void)session; return storage_available(volume); }

static file_t *find_file(const char *path, bool create)
{
    for (unsigned i = 0; i < 8; ++i)
        if (files[i].exists && !strcmp(files[i].path, path)) return &files[i];
    if (create)
        for (unsigned i = 0; i < 8; ++i)
            if (!files[i].exists)
            {
                memset(&files[i], 0, sizeof(files[i]));
                snprintf(files[i].path, sizeof(files[i].path), "%s", path);
                files[i].exists = true;
                return &files[i];
            }
    return NULL;
}
static int stat(const char *path, struct stat *st)
{
    if (io_failure == IO_STAT) { errno = -EIO; return -1; }
    if (!strcmp(path, "/flash/system")) { st->st_mode = S_DIR; st->st_size = 0; return 0; }
    if (strstr(path, "/demo/app.json")) { st->st_mode = S_REG; st->st_size = 1; return 0; }
    file_t *file = find_file(path, false);
    if (!file) { errno = -ENOENT; return -1; }
    st->st_mode = S_REG;
    st->st_size = file->size;
    return 0;
}
static int mkdir(const char *path, int mode) { (void)path; (void)mode; return 0; }
static int open(const char *path, int flags, ...)
{
    if (((flags & O_WRONLY) && io_failure == IO_OPEN_WRITE) ||
        (!(flags & O_WRONLY) && io_failure == IO_OPEN_READ)) return -1;
    file_t *file = find_file(path, (flags & O_CREAT) != 0);
    if (!file) { errno = -ENOENT; return -1; }
    if (flags & O_TRUNC) file->size = 0;
    for (int i = 0; i < 8; ++i)
        if (!descriptors[i].active)
        {
            descriptors[i] = (fd_t){file, 0, (flags & O_WRONLY) != 0, true};
            return i;
        }
    assert(false);
    return -1;
}
static int fstat(int fd, struct stat *st)
{
    if (io_failure == IO_FSTAT) return -1;
    st->st_mode = S_REG;
    st->st_size = descriptors[fd].file->size;
    return 0;
}
static int read(int fd, void *buffer, size_t size)
{
    if (io_failure == IO_READ) return -1;
    fd_t *item = &descriptors[fd];
    if (size > item->file->size - item->position) size = item->file->size - item->position;
    memcpy(buffer, item->file->data + item->position, size);
    item->position += size;
    return (int)size;
}
static int write(int fd, const void *buffer, size_t size)
{
    ++writes;
    fd_t *item = &descriptors[fd];
    /* Fail after a short write, exercising the real transfer loop. */
    if (io_failure == IO_WRITE)
    {
        if (item->position) return -1;
        if (size > 3) size = 3;
    }
    assert(item->position + size <= sizeof(item->file->data));
    memcpy(item->file->data + item->position, buffer, size);
    item->position += size;
    item->file->size = item->position;
    return (int)size;
}
static int fsync(int fd) { (void)fd; return io_failure == IO_SYNC ? -1 : 0; }
static int close(int fd)
{
    assert(descriptors[fd].active);
    descriptors[fd].active = false;
    return io_failure == IO_CLOSE && descriptors[fd].writing ? -1 : 0;
}
static int test_unlink(const char *path)
{ file_t *file = find_file(path, false); if (file) file->exists = false; return 0; }
static int test_rename(const char *from, const char *to)
{
    if (io_failure == IO_RENAME) return -1;
    file_t *file = find_file(from, false);
    assert(file);
    test_unlink(to);
    snprintf(file->path, sizeof(file->path), "%s", to);
    return 0;
}
#define unlink test_unlink
#define rename test_rename
#include "storage_under_test.h"

#ifdef RT_USING_FINSH
#define MSH_CMD_EXPORT(command, description) \
    static int (*exported_recovery)(int, char **) = command
#endif
#include "recovery_under_test.h"

static unsigned package_reads, verifications, loader_calls, module_init_calls, service_starts;
static unsigned directory_opens, font_reserves, closes;
static bool service_running, installer_busy, service_stopping;
static bool metadata_background = true, metadata_resources;
static bool (*registered_open)(const char *, app_entity_info *);
static bool (*registered_close)(const char *, const app_entity_info *);
static const builtin_app_desc_t builtin = {"settings", "Settings", (void *)1};
static const builtin_app_desc_t *gui_builtin_app_list_open(void) { return &builtin; }
static const builtin_app_desc_t *gui_builtin_app_list_get_next(const builtin_app_desc_t *item)
{ (void)item; return NULL; }
static void gui_builtin_app_list_close(const builtin_app_desc_t *item) { (void)item; }
static void gui_app_register_dl_open(bool (*callback)(const char *, app_entity_info *)) { registered_open = callback; }
static void gui_app_register_dl_close(bool (*callback)(const char *, const app_entity_info *)) { registered_close = callback; }
static gui_runing_app_t *gui_app_trav(rt_list_t **cursor) { (void)cursor; return NULL; }
static void gui_app_exit(const char *id) { (void)id; }
static unsigned exec_calls;
static void gui_app_exec_now(void) { ++exec_calls; }
bool app_installer_busy(void) { return installer_busy; }
bool app_installer_blocks(const char *id) { (void)id; return false; }
bool app_installer_volume_ready(storage_volume_t volume) { (void)volume; return true; }
void app_installer_process(void) {}
bool storage_changing(void) { return false; }
bool storage_apps_enabled(storage_volume_t volume) { (void)volume; return true; }
bool app_service_stopping(void) { return service_stopping; }
bool app_service_running(const char *id) { (void)id; return service_running; }
void app_service_stop_modules(void) {}
void app_service_process(void) {}
bool app_service_modules_running(void) { return service_running; }
bool app_modules_present(void) { return service_running; }
bool app_package_metadata(const char *root, const char *id, epd_app_entry_t *entry)
{
    (void)root;
    ++package_reads;
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->id, sizeof(entry->id), "%s", id);
    snprintf(entry->name, sizeof(entry->name), "%s", id);
    entry->background = metadata_background;
    entry->resources_only = metadata_resources;
    return true;
}
app_package_status_t app_package_verify(const char *root, const char *id, bool all_files)
{ (void)root; (void)id; (void)all_files; ++verifications; return APP_PACKAGE_OK; }
const char *app_package_error(app_package_status_t status) { (void)status; return "test"; }
void *epd_app_realloc(void *items, size_t size, epd_app_memory_t region)
{ (void)region; return realloc(items, size); }
void epd_app_free(void *items) { free(items); }
static void ui_reader_font_cache_reserve(size_t bytes) { assert(bytes == 2u * 1024u * 1024u); ++font_reserves; }
struct rt_dlmodule { int unused; };
static struct rt_dlmodule *dlmodule_find(const char *id) { (void)id; return NULL; }
void *app_module_open(const char *path, const char *id)
{
    assert(!locks && !irq_depth);
    assert(strstr(path, "/demo.so") && !strcmp(id, "demo"));
    ++loader_calls;
    /* The SDK invokes module_init from inside dlopen, before returning. */
    ++module_init_calls;
    return (void *)(uintptr_t)0x1000;
}
void app_module_close(void *module) { assert(module == (void *)(uintptr_t)0x1000); ++closes; }
#define EPD_APP_ABI 1u
typedef struct { uint32_t abi; const char *id; } epd_app_info_t;
static const epd_app_info_t mock_descriptor = {EPD_APP_ABI, "demo"};
static const epd_background_t mock_background = {0};
static void *dlsym(void *module, const char *name)
{
    assert(module == (void *)(uintptr_t)0x1000);
    if (!strcmp(name, "epd_app_info")) return (void *)&mock_descriptor;
    if (!strcmp(name, "app_main")) return (void *)(uintptr_t)0x2000;
    assert(!strcmp(name, "epd_app_background"));
    return (void *)&mock_background;
}
epd_service_t *app_service_start(const char *id, const epd_background_t *definition, void *module)
{
    assert(!locks && !irq_depth && !strcmp(id, "demo") && definition == &mock_background && module);
    ++service_starts;
    service_running = true;
    return (epd_service_t *)(uintptr_t)1;
}
struct dirent { char d_name[32]; };
typedef struct { bool consumed; } DIR;
static DIR mock_directory;
static DIR *opendir(const char *path)
{
    ++directory_opens;
    if (strcmp(path, STORAGE_FLASH_APPS)) return NULL;
    mock_directory.consumed = false;
    return &mock_directory;
}
static struct dirent *readdir(DIR *dir)
{
    static struct dirent item = {"demo"};
    if (dir->consumed) return NULL;
    dir->consumed = true;
    return &item;
}
static void closedir(DIR *dir) { (void)dir; }
#define LOG_W(...) rt_kprintf(__VA_ARGS__)
#define LOG_I(...) rt_kprintf(__VA_ARGS__)
#define DBG_INFO 1
#include "catalog_under_test.h"

typedef struct { int unused; } lv_obj_t;
typedef struct { ui_page_id_t id; void *feature, *popup; bool foreground; lv_obj_t *popup_button; } page_t;
static page_t page;
static page_t *current = &page;
static char popup_message[128];
static unsigned intents, installs, uninstalls;
#define NAV_BACK (-1)
static void list_refresh(void) {}
static void popup_open(const char *message, const char *detail)
{ (void)detail; snprintf(popup_message, sizeof(popup_message), "%s", message); current->popup = (void *)1; }
static void popup_close(void) { current->popup = NULL; }
static void popup_confirm(const char *message, const char *detail, int target)
{ (void)target; popup_open(message, detail); }
static void object_visible(lv_obj_t *object, bool visible) { (void)object; (void)visible; }
static intent_t intent_init(const char *id) { assert(!strcmp(id, "demo")); ++intents; return (void *)1; }
static void intent_set_string(intent_t intent, const char *key, const char *value)
{ assert(intent && !strcmp(key, "page") && !strcmp(value, "settings")); }
static int intent_runapp(intent_t intent) { assert(intent); return RT_EOK; }
static void intent_deinit(intent_t intent) { assert(intent); }
static bool gui_app_is_actived(char *id) { return !strcmp(id, "demo"); }
static void select_install_volume(const char *name) { (void)name; current->popup = (void *)1; }
bool app_installer_start(const char *id, bool uninstall, storage_volume_t volume, uint32_t *ticket)
{
    assert(!strcmp(id, "demo") && volume == STORAGE_FLASH);
    if (uninstall) ++uninstalls; else ++installs;
    *ticket = 1;
    return true;
}
#include "management_under_test.h"

static void reboot(void)
{
    assert(!locks && !irq_depth);
    for (unsigned i = 0; i < 8; ++i) assert(!descriptors[i].active);
    recovery_state = (app_recovery_status_t){false, true, false, false, -RT_EBUSY};
    loader_calls = module_init_calls = service_starts = directory_opens = 0;
    package_reads = verifications = font_reserves = closes = intents = exec_calls = 0;
    service_running = installer_busy = service_stopping = false;
    background_revision = UINT32_MAX;
    catalog_revision = 0;
    free(entries);
    entries = NULL;
    entry_count = 0;
    shell_output[0] = 0;
}
static void fresh(void)
{
    reboot();
    memset(files, 0, sizeof(files));
    io_failure = IO_OK;
    flash_available = true;
    writes = 0;
    metadata_background = true;
    metadata_resources = false;
}
static void seed(uint8_t value)
{ assert(storage_record_save(RECOVERY_PATH, RECOVERY_MAGIC, RECOVERY_VERSION, &value, sizeof(value))); }
static void test_normal(void)
{
    fresh();
    assert(app_recovery_active() && app_recovery_check_dynamic() == -RT_EBUSY);
    assert(app_recovery_set_next_boot(true) == -RT_EBUSY);
    app_catalog_init();
    assert(registered_open && registered_close && !app_recovery_active() && writes == 0);
    assert(app_recovery_check_dynamic() == RT_EOK);
    app_catalog_process();
    assert(loader_calls == 1 && module_init_calls == 1 && service_starts == 1 && closes == 1);
    app_catalog_process();
    assert(loader_calls == 1);
    app_entity_info info;
    assert(registered_open("demo", &info) && info.entry_func && info.user_data);
    assert(registered_close("demo", &info) && !registered_close("settings", &info));
    assert(loader_calls == 2 && service_starts == 1);
    puts("PASS normal: absent flag preserves automatic/manual loading and references");
}
static void test_recovery(void)
{
    fresh();
    seed(1);
    app_catalog_init();
    assert(app_recovery_active());
    app_catalog_process();
    assert(!directory_opens && background_revision == UINT32_MAX);
    app_entity_info info = {123, 456};
    assert(!registered_open("demo", &info));
    assert(!info.entry_func && !info.user_data && errno == APP_RECOVERY_BLOCKED);
    assert(!package_reads && !verifications && !loader_calls && !module_init_calls && !service_starts && !font_reserves);
    app_catalog_refresh();
    assert(app_catalog_count() == 2 && !strcmp(app_catalog_get(0)->id, "settings"));
    epd_app_entry_t entry;
    assert(app_package_read(STORAGE_FLASH_APPS, "demo", &entry));
    metadata_resources = true;
    assert(app_package_read(STORAGE_FLASH_APPS, "settings", &entry));
    metadata_resources = false;
    app_catalog_changed();
    app_catalog_process();
    assert(!loader_calls && !module_init_calls && !service_starts && app_catalog_release_storage());
    puts("PASS recovery: manual/background loading blocked before module_init/services; metadata stays readable");
}
static void test_restart_semantics(void)
{
    fresh();
    app_catalog_init();
    assert(app_recovery_set_next_boot(true) == RT_EOK);
    assert(!app_recovery_active() && app_recovery_init() == RT_EOK);
    app_recovery_status_t status;
    app_recovery_get_status(&status);
    assert(!status.active && status.next_boot && status.next_boot_valid);
    app_catalog_process();
    assert(service_starts == 1);
    reboot();
    app_catalog_init();
    assert(app_recovery_active());
    assert(app_recovery_set_next_boot(false) == RT_EOK);
    assert(app_recovery_init() == RT_EOK && app_recovery_active());
    app_catalog_init();
    app_catalog_process();
    app_entity_info info;
    assert(!registered_open("demo", &info) && !loader_calls && !service_starts);
    app_recovery_get_status(&status);
    assert(status.active && !status.next_boot && status.next_boot_valid);
    reboot();
    app_catalog_init();
    assert(!app_recovery_active());
    app_catalog_process();
    assert(module_init_calls == 1 && service_starts == 1);
    puts("PASS restart: on/off applies next boot only; repeated init cannot re-enable code");
}
static void test_write_failures(void)
{
    const int failures[] = {IO_OPEN_WRITE, IO_WRITE, IO_SYNC, IO_CLOSE, IO_RENAME};
    for (unsigned enabled = 0; enabled < 2; ++enabled)
        for (unsigned i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i)
        {
            fresh();
            seed((uint8_t)enabled);
            assert(app_recovery_init() == RT_EOK);
            file_t before = *find_file(RECOVERY_PATH, false);
            io_failure = failures[i];
            assert(app_recovery_set_next_boot(!enabled) == -RT_EIO);
            assert(app_recovery_active() == (app_recovery_forced() || enabled != 0));
            app_recovery_status_t status;
            app_recovery_get_status(&status);
            assert(status.next_boot == (enabled != 0) && status.next_boot_valid);
            file_t *after = find_file(RECOVERY_PATH, false);
            assert(after && before.size == after->size && !memcmp(before.data, after->data, before.size));
            io_failure = IO_OK;
            reboot();
            assert(app_recovery_init() == RT_EOK &&
                   app_recovery_active() == (app_recovery_forced() || enabled != 0));
        }
    fresh();
    assert(app_recovery_init() == RT_EOK);
    io_failure = IO_RENAME;
    assert(app_recovery_set_next_boot(true) == -RT_EIO && !find_file(RECOVERY_PATH, false));
    io_failure = IO_OK;
    reboot();
    assert(app_recovery_init() == RT_EOK && app_recovery_active() == app_recovery_forced());
    puts("PASS write failures: open/short-write/fsync/close/rename preserve record and mode");
}
static void test_read_failures(void)
{
    const int failures[] = {IO_STAT, IO_OPEN_READ, IO_READ, IO_FSTAT};
    for (unsigned i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i)
    {
        fresh();
        seed(0);
        io_failure = failures[i];
        assert(app_recovery_init() == -RT_EIO && app_recovery_active());
        app_recovery_status_t status;
        app_recovery_get_status(&status);
        assert(!status.next_boot_valid);
        io_failure = IO_OK;
        assert(app_recovery_init() == -RT_EIO && app_recovery_active());
    }
    fresh();
    flash_available = false;
    assert(app_recovery_init() == -RT_EIO && app_recovery_active());
    assert(app_recovery_set_next_boot(false) == -RT_EIO);
    flash_available = true;
    assert(app_recovery_set_next_boot(false) == RT_EOK && app_recovery_active());
    reboot();
    assert(app_recovery_init() == RT_EOK && app_recovery_active() == app_recovery_forced());
    fresh();
    seed(1);
    find_file(RECOVERY_PATH, false)->data[16] ^= 1; /* Break the real record CRC. */
    assert(app_recovery_init() == -RT_EIO && app_recovery_active());
    assert(app_recovery_set_next_boot(false) == RT_EOK && app_recovery_active());
    reboot();
    assert(app_recovery_init() == RT_EOK && app_recovery_active() == app_recovery_forced());
    fresh();
    seed(2); /* Valid record with an invalid mode byte. */
    assert(app_recovery_init() == -RT_EIO && app_recovery_active());
    puts("PASS read errors: unreadable/invalid records block code; repair applies after restart");
}
static void test_settings(void)
{
    fresh();
    seed(1);
    app_catalog_init();
    app_list_t list = {0};
    page = (page_t){UI_PAGE_APP_SETTINGS, &list, NULL, true, NULL};
    list.count = 1;
    snprintf(list.entries[0].id, sizeof(list.entries[0].id), "demo");
    list.pages[0] = UI_PAGE_COUNT;
    int target = APP_ROW_BASE;
    assert(app_list_action(&target) && !intents && !exec_calls);
    assert(!strcmp(popup_message, "恢复模式下无法打开应用"));
    assert(!open_dynamic_settings("demo") && !intents && errno == APP_RECOVERY_BLOCKED);
    list.pages[0] = UI_PAGE_DISPLAY_SETTINGS;
    target = APP_ROW_BASE;
    assert(!app_list_action(&target) && target == UI_PAGE_DISPLAY_SETTINGS);
    for (unsigned uninstall = 0; uninstall < 2; ++uninstall)
    {
        page.id = uninstall ? UI_PAGE_APP_UNINSTALL : UI_PAGE_APP_INSTALL;
        list.ticket = 0;
        target = APP_ROW_BASE;
        assert(app_list_action(&target) && current->popup);
        target = APP_CONFIRM;
        assert(app_list_action(&target) && list.ticket);
    }
    assert(installs == 1 && uninstalls == 1 && !loader_calls && !service_starts);
    fresh();
    app_catalog_init();
    if (app_recovery_forced()) assert(!open_dynamic_settings("demo") && !intents);
    else assert(open_dynamic_settings("demo") && intents == 1);
    puts("PASS settings: built-in navigation/install/uninstall stay usable; dynamic settings blocked");
}
#ifdef RT_USING_FINSH
static void test_finsh(void)
{
    fresh();
    char *on[] = {"recovery", "on"}, *off[] = {"recovery", "off"};
    char *status[] = {"recovery", "status"}, *invalid[] = {"recovery", "invalid"};
    assert(exported_recovery(2, on) == -RT_EBUSY && !writes);
    assert(exported_recovery(2, invalid) == -RT_EINVAL && !writes);
    app_catalog_init();
    shell_output[0] = 0;
    assert(exported_recovery(1, status) == RT_EOK && !writes);
    assert(strstr(shell_output, app_recovery_forced() ?
                  "current=recovery next=normal load_result=0 forced=1" :
                  "current=normal next=normal load_result=0 forced=0"));
    shell_output[0] = 0;
    assert(exported_recovery(2, on) == RT_EOK && app_recovery_active() == app_recovery_forced());
    assert(strstr(shell_output, "next=recovery"));
    assert(strstr(shell_output, app_recovery_forced() ? "use normal firmware to apply" : "restart manually"));
    io_failure = IO_SYNC;
    shell_output[0] = 0;
    assert(exported_recovery(2, off) == -RT_EIO);
    assert(strstr(shell_output, "save failed") && strstr(shell_output, "next=recovery"));
    io_failure = IO_OK;
    reboot();
    app_catalog_init();
    assert(exported_recovery(2, off) == RT_EOK && app_recovery_active());
    reboot();
    app_catalog_init();
    assert(app_recovery_active() == app_recovery_forced());
    puts("PASS finsh: export, argument errors, failure reporting and explicit re-enable");
}
#endif
static void test_forced_firmware(void)
{
    fresh();
    app_catalog_init();
    assert(app_recovery_forced() && app_recovery_active() && !writes);
    app_recovery_status_t status;
    app_recovery_get_status(&status);
    assert(status.next_boot_valid && !status.next_boot);
    seed(0);
    reboot();
    app_catalog_init();
    assert(app_recovery_check_dynamic() == APP_RECOVERY_BLOCKED);
    assert(app_recovery_set_next_boot(false) == RT_EOK);
    app_recovery_get_status(&status);
    assert(status.active && !status.next_boot && status.next_boot_valid);
    assert(app_recovery_init() == RT_EOK && app_recovery_active());
    app_entity_info info;
    assert(!registered_open("demo", &info));
    app_catalog_process();
    assert(!loader_calls && !module_init_calls && !service_starts);
    reboot();
    app_catalog_init();
    assert(app_recovery_active());
    uint8_t saved = 1;
    assert(storage_record_load(RECOVERY_PATH, RECOVERY_MAGIC, RECOVERY_VERSION, &saved, sizeof(saved)));
    assert(saved == 0);
    puts("PASS forced firmware: missing/off flags and recovery off never allow code or silently change the saved flag");
}
int main(void)
{
    if (app_recovery_forced()) test_forced_firmware();
    else { test_normal(); test_restart_semantics(); }
    test_recovery();
    test_write_failures();
    test_read_failures();
    test_settings();
#ifdef RT_USING_FINSH
    test_finsh();
#endif
    reboot();
    puts("Recovery tests passed");
    return 0;
}
