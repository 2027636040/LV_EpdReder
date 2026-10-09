#include "app_installer.h"
#include "app_catalog.h"
#include "app_package.h"
#include "storage.h"
#include "storage_file.h"
#include "app_service.h"
#include "gui_app_int.h"
#include <dlmodule.h>
#include <errno.h>
#include <dfs_posix.h>
#include <dfs_fs.h>
#include <string.h>
#include <stdio.h>

typedef struct
{
    storage_volume_t volume;
    const char *root, *directory, *staging, *removing, *backup, *journal;
} install_paths_t;
static const install_paths_t paths[STORAGE_COUNT] = {
    {STORAGE_FLASH, STORAGE_FLASH_ROOT, STORAGE_FLASH_APPS,
     "/flash/.app-install", "/flash/.app-remove", "/flash/.app-old", "/flash/system/app-transaction.bin"},
    {STORAGE_SD, STORAGE_SD_ROOT "/.epd", STORAGE_SD_APPS,
     "/sdcard/.epd/.app-install", "/sdcard/.epd/.app-remove", "/sdcard/.epd/.app-old",
     "/sdcard/.epd/app-transaction.bin"}
};
#define COPY_SIZE 4096
#define PACKAGE_DEPTH_MAX 8

static app_job_result_t result;
static char job_id[GUI_APP_ID_MAX_LEN];
static bool remove_job;
static storage_volume_t job_volume;
static uint32_t job_session;
static volatile bool recovery_running;
static bool recovery_pending;

void app_installer_result(app_job_result_t *out)
{
    rt_base_t level = rt_hw_interrupt_disable();
    *out = result;
    rt_hw_interrupt_enable(level);
}
bool app_installer_busy(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool busy = result.state == APP_JOB_RUNNING || recovery_running;
    rt_hw_interrupt_enable(level);
    return busy;
}

static bool join_path(char *out, size_t size, const char *base, const char *name)
{
    int n = snprintf(out, size, "%s/%s", base, name);
    return n > 0 && n < size;
}

/* One buffer is shared through recursion; paths and DIR handles are bounded. */
static bool copy_file(const char *from, const char *to, void *buffer)
{
    int src = open(from, O_RDONLY), dst = -1;
    if (src < 0) return false;
    dst = open(to, O_CREAT | O_EXCL | O_WRONLY, 0);
    struct stat st;
    bool ok = dst >= 0 && fstat(src, &st) == 0;
    uint64_t copied = 0;
    int n = 0;
    while (ok && storage_available(STORAGE_SD) && (n = read(src, buffer, COPY_SIZE)) > 0)
    {
        int offset = 0;
        while (offset < n)
        {
            int written = write(dst, (char *)buffer + offset, n - offset);
            if (written <= 0) { ok = false; break; }
            offset += written;
            copied += written;
        }
    }
    if (n < 0 || !storage_available(STORAGE_SD)) ok = false;
    if (ok && copied != (uint64_t)st.st_size) ok = false;
    if (ok && fsync(dst) != 0) ok = false;
    if (dst >= 0 && close(dst) != 0) ok = false;
    close(src);
    return ok;
}

typedef enum { TREE_SIZE, TREE_COPY, TREE_REMOVE } tree_operation_t;
static bool tree_walk(const char *from, const char *to, unsigned depth,
                      tree_operation_t operation, uint32_t block_size, uint64_t *bytes, void *buffer)
{
    if (depth > PACKAGE_DEPTH_MAX || !storage_path_available(from)) return false;
    if (operation != TREE_REMOVE && !storage_available(STORAGE_SD)) return false;
    struct stat st;
    if (stat(from, &st) != 0) return false;
    if (S_ISREG(st.st_mode))
    {
        if (operation == TREE_REMOVE) return unlink(from) == 0;
        *bytes += ((uint64_t)st.st_size + block_size - 1) / block_size * block_size;
        if (operation == TREE_COPY) return copy_file(from, to, buffer);
        return true;
    }
    if (!S_ISDIR(st.st_mode)) return false;
    if (operation == TREE_COPY && mkdir(to, 0) != 0) return false;
    if (operation != TREE_REMOVE) *bytes += block_size;
    DIR *dir = opendir(from);
    if (!dir) return false;
    bool ok = true;
    struct dirent *entry;
    while (ok && (entry = readdir(dir)) != NULL)
    {
        if (!storage_path_available(from)) { ok = false; break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char source[256], destination[256];
        ok = join_path(source, sizeof(source), from, entry->d_name);
        if (operation == TREE_COPY) ok = ok && join_path(destination, sizeof(destination), to, entry->d_name);
        if (ok) ok = tree_walk(source, operation == TREE_COPY ? destination : NULL,
                               depth + 1, operation, block_size, bytes, buffer);
    }
    closedir(dir);
    if (ok && operation == TREE_REMOVE) ok = rmdir(from) == 0;
    return ok;
}

static bool clear_stage(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return errno == -ENOENT;
    return tree_walk(path, NULL, 0, TREE_REMOVE, 0, NULL, NULL);
}

typedef enum { TX_COPY = 1, TX_PUBLISH, TX_COMMIT, TX_REMOVE, TX_REMOVED } transaction_phase_t;
typedef struct { uint32_t phase; char id[RT_NAME_MAX]; } transaction_t;
#define JOURNAL_MAGIC 0x41505054u
static struct rt_semaphore prepared;
static bool recovery_ready[STORAGE_COUNT], needs_prepare, prepare_ok, exit_sent;
static uint32_t recovery_session;
static bool prepared_resources;
static rt_tick_t prepare_started;

static bool volume_ready_set(storage_volume_t volume, bool ready)
{
    rt_base_t level = rt_hw_interrupt_disable();
    recovery_ready[volume] = ready;
    if (volume == STORAGE_SD && ready) recovery_session = storage_card_session();
    rt_hw_interrupt_enable(level);
    return ready;
}

static int path_state(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) return 1;
    return errno == -ENOENT ? 0 : -1;
}

/* FAT does not replace an existing file with rename. Append checksummed phases
 * and reuse a torn tail; the preceding complete phase remains recoverable. */
typedef struct { uint32_t magic; transaction_t transaction; uint32_t crc; } sd_journal_t;
static bool sd_transaction_read(const char *path, transaction_t *transaction, off_t *length)
{
    sd_journal_t record;
    *length = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    while (storage_path_available(path) && read(fd, &record, sizeof(record)) == sizeof(record) && record.magic == JOURNAL_MAGIC &&
           record.crc == storage_crc32(&record.transaction, sizeof(record.transaction)))
    {
        *transaction = record.transaction;
        *length += sizeof(record);
    }
    close(fd);
    return *length != 0;
}

static bool save_transaction(const install_paths_t *p, transaction_t *transaction, transaction_phase_t phase)
{
    transaction->phase = phase;
    if (!storage_available(p->volume)) return false;
    if (p->volume == STORAGE_FLASH)
        return storage_record_save(p->journal, JOURNAL_MAGIC, 1, transaction, sizeof(*transaction));
    off_t length;
    transaction_t previous;
    sd_transaction_read(p->journal, &previous, &length);
    sd_journal_t record = {JOURNAL_MAGIC, *transaction, storage_crc32(transaction, sizeof(*transaction))};
    int fd = open(p->journal, O_CREAT | O_WRONLY, 0);
    if (fd < 0) return false;
    bool ok = lseek(fd, length, SEEK_SET) == length &&
              write(fd, &record, sizeof(record)) == sizeof(record) && fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    return ok;
}

static bool valid_id(const char *id)
{
    size_t n = strlen(id);
    if (!n || n >= RT_NAME_MAX) return false;
    for (const char *p = id; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return false;
    return true;
}

/* Called with storage locked and no live module from this transaction. Every
 * rename stays on the target volume. FAT journals do not use file replacement. */
static bool recover_locked(const install_paths_t *p, bool *published)
{
    transaction_t transaction;
    if (published) *published = false;
    if (!storage_available(p->volume)) return false;
    int journal = path_state(p->journal), backup = path_state(p->backup);
    if (journal < 0 || backup < 0) return false;
    if (!journal)
        return !backup && clear_stage(p->staging) && clear_stage(p->removing);
    off_t length;
    bool loaded = p->volume == STORAGE_FLASH ?
        storage_record_load(p->journal, JOURNAL_MAGIC, 1, &transaction, sizeof(transaction)) :
        sd_transaction_read(p->journal, &transaction, &length);
    /* No complete first phase means publication/removal could not have begun. */
    if (!loaded && p->volume == STORAGE_SD && !backup && path_state(p->removing) == 0)
        return clear_stage(p->staging) && unlink(p->journal) == 0;
    if (!loaded ||
        !memchr(transaction.id, 0, sizeof(transaction.id)) || !valid_id(transaction.id)) return false;
    char destination[96], cache[96];
    snprintf(destination, sizeof(destination), "%s/%s", p->directory, transaction.id);
    snprintf(cache, sizeof(cache), "%s/cache/%s", p->root, transaction.id);
    int installed = path_state(destination), staging = path_state(p->staging), removing = path_state(p->removing);
    if (installed < 0 || staging < 0 || removing < 0) return false;
    bool committed = false;
    switch (transaction.phase)
    {
    case TX_COPY:
        if (backup) return false;
        if (!clear_stage(p->staging)) return false;
        break;
    case TX_PUBLISH:
        if (staging)
        {
            if (app_package_verify(p->staging, transaction.id, true) != APP_PACKAGE_OK)
            {
                /* Copy was not committed. Restore an intact old directory. */
                if (backup && (installed || rename(p->backup, destination) != 0)) return false;
                if (!save_transaction(p, &transaction, TX_COPY) || !clear_stage(p->staging)) return false;
                break;
            }
            if (installed)
            {
                if (backup || rename(destination, p->backup) != 0) return false;
            }
            if (rename(p->staging, destination) != 0) return false;
            installed = 1;
        }
        else if (!installed) return false;
        if (!save_transaction(p, &transaction, TX_COMMIT)) return false;
        /* fall through */
    case TX_COMMIT:
        if (!installed) return false;
        if (!clear_stage(p->backup) || !clear_stage(cache)) return false;
        committed = true;
        break;
    case TX_REMOVE:
        if (installed)
        {
            if (removing || rename(destination, p->removing) != 0) return false;
        }
        if (!save_transaction(p, &transaction, TX_REMOVED)) return false;
        /* fall through */
    case TX_REMOVED:
        if (!clear_stage(p->removing) || !clear_stage(cache)) return false;
        break;
    default:
        return false;
    }
    if (unlink(p->journal) != 0) return false;
    if (published) *published = committed;
    return true;
}

/* Only a quiescent application can migrate. The published target is never
 * overwritten; a marker permits cleanup after a completed directory rename. */
static bool migration_equal(const char *source, const char *destination, unsigned depth)
{
    struct stat st;
    if (depth > PACKAGE_DEPTH_MAX || !storage_path_available(source) ||
        !storage_path_available(destination) || stat(source, &st) != 0) return false;
    if (S_ISREG(st.st_mode))
    {
        struct stat target;
        uint32_t a, b;
        return stat(destination, &target) == 0 && S_ISREG(target.st_mode) && st.st_size == target.st_size &&
               storage_file_signature(source, &a) && storage_file_signature(destination, &b) && a == b;
    }
    if (!S_ISDIR(st.st_mode)) return false;
    DIR *dir = opendir(source);
    if (!dir) return false;
    bool ok = true;
    struct dirent *entry;
    while (ok && (entry = readdir(dir)) != NULL)
    {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char from[256], to[256];
        ok = join_path(from, sizeof(from), source, entry->d_name) &&
             join_path(to, sizeof(to), destination, entry->d_name) && migration_equal(from, to, depth + 1);
    }
    closedir(dir);
    return ok;
}

static bool migrate_data(const char *id, storage_volume_t volume)
{
    char source[96], target[96], stage[96], marker[112], other_app[96];
    const install_paths_t *to = &paths[volume], *from = &paths[volume == STORAGE_SD ? STORAGE_FLASH : STORAGE_SD];
    if (!storage_available(volume)) return false;
    snprintf(target, sizeof(target), "%s/data/%s", to->root, id);
    snprintf(source, sizeof(source), "%s/data/%s", from->root, id);
    snprintf(other_app, sizeof(other_app), "%s/%s", from->directory, id);
    if (!storage_available(from->volume) || path_state(other_app) != 0) return true;
    if (path_state(source) == 0) return true;
    if (path_state(source) < 0) return false;
    snprintf(stage, sizeof(stage), "%s/.data-%s", to->root, id);
    snprintf(marker, sizeof(marker), "%s/.migrating", target);
    int installed = path_state(target);
    if (installed < 0) return false;
    if (installed && path_state(marker) == 0) return true;
    if (dlmodule_find(id) || app_service_running(id)) return false;
    const uint32_t magic = 0x4d494752u;
    if (installed)
    {
        uint32_t saved = 0;
        int fd = open(marker, O_RDONLY);
        bool valid = fd >= 0 && read(fd, &saved, sizeof(saved)) == sizeof(saved) && saved == magic;
        if (fd >= 0) close(fd);
        if (!valid) return false;
    }
    if (!installed)
    {
        char parent[96];
        snprintf(parent, sizeof(parent), "%s/data", to->root);
        if (!storage_mkdirs(parent) || !clear_stage(stage)) return false;
        void *buffer = rt_malloc(COPY_SIZE);
        if (!buffer) return false;
        uint64_t copied = 0;
        bool ok = tree_walk(source, stage, 0, TREE_COPY, 1, &copied, buffer) && migration_equal(source, stage, 0);
        rt_free(buffer);
        char stage_marker[112];
        snprintf(stage_marker, sizeof(stage_marker), "%s/.migrating", stage);
        if (ok) ok = storage_file_replace(stage_marker, &magic, sizeof(magic)) && rename(stage, target) == 0;
        if (!ok) return false;
    }
    /* A conflicting source stays intact. On retry, the remaining source may be
     * a subset of the already published target after interrupted cleanup. */
    if (migration_equal(source, target, 0) && !clear_stage(source)) return false;
    if (!storage_path_available(target) || unlink(marker) != 0) return false;
    return true;
}

static bool prepare_data(storage_volume_t volume)
{
    if (volume == STORAGE_FLASH)
    {
        int old = path_state("/flash/data/reader"), next = path_state("/flash/data/books");
        if (old < 0 || next < 0 || (old && !next && rename("/flash/data/reader", "/flash/data/books") != 0))
            return false;
    }
    if (volume == STORAGE_SD && !storage_apps_enabled(volume)) return true;
    const char *root = storage_app_directory(volume);
    DIR *dir = opendir(root);
    if (!dir) return path_state(root) == 0;
    bool ok = true;
    struct dirent *item;
    while (ok && (item = readdir(dir)) != NULL)
    {
        if (!valid_id(item->d_name)) continue;
        char metadata[128];
        snprintf(metadata, sizeof(metadata), "%s/%s/app.json", root, item->d_name);
        if (path_state(metadata) == 1) ok = migrate_data(item->d_name, volume);
    }
    closedir(dir);
    return ok;
}

void app_installer_recover(void)
{
    rt_sem_init(&prepared, "app_pre", 0, RT_IPC_FLAG_FIFO);
    storage_lock();
    for (storage_volume_t v = STORAGE_FLASH; v < STORAGE_COUNT; ++v)
        volume_ready_set(v, storage_available(v) && recover_locked(&paths[v], NULL) &&
                           (v != STORAGE_FLASH || prepare_data(v)));
    storage_unlock();
    if (!recovery_ready[STORAGE_FLASH]) rt_kprintf("[apps] internal transaction recovery incomplete\n");
}

bool app_installer_volume_ready(storage_volume_t volume)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool ready = (unsigned)volume < STORAGE_COUNT && storage_available(volume) && recovery_ready[volume] &&
                 (volume != STORAGE_SD || storage_session_valid(volume, recovery_session));
    rt_hw_interrupt_enable(level);
    return ready;
}

static void recover_card(void *parameter)
{
    (void)parameter;
    storage_lock();
    uint32_t session = storage_card_session();
    bool ready = volume_ready_set(STORAGE_SD, recover_locked(&paths[STORAGE_SD], NULL) &&
                           prepare_data(STORAGE_SD) && storage_session_valid(STORAGE_SD, session));
    storage_unlock();
    if (!ready && storage_available(STORAGE_SD)) rt_kprintf("[apps] TF transaction recovery incomplete\n");
    recovery_running = false;
    app_catalog_changed();
}

void app_installer_storage_changed(void)
{
    volume_ready_set(STORAGE_SD, false);
    recovery_pending = storage_available(STORAGE_SD);
}

static void start_card_recovery(void)
{
    if (!recovery_pending || app_installer_busy() || storage_changing()) return;
    if (!storage_available(STORAGE_SD)) { recovery_pending = false; return; }
    rt_thread_t thread = rt_thread_create("apprecv", recover_card, NULL, 12288, 23, 10);
    if (!thread) return;
    recovery_running = true;
    recovery_pending = false;
    if (rt_thread_startup(thread) != RT_EOK)
    { recovery_running = false; recovery_pending = true; rt_thread_delete(thread); }
}

bool app_installer_blocks(const char *id)
{
    storage_volume_t volume;
    if (storage_release_requested() || storage_changing() || !storage_app_locate(id, &volume) || !app_installer_volume_ready(volume) ||
        !storage_apps_enabled(volume)) return true;
    rt_base_t level = rt_hw_interrupt_disable();
    bool blocked = (result.state == APP_JOB_RUNNING && !strcmp(id, job_id));
    rt_hw_interrupt_enable(level);
    return blocked;
}

/* Only the UI thread calls app_fwk. The installer worker never executes a page
 * callback or waits for a service while holding the storage mutex. */
void app_installer_process(void)
{
    start_card_recovery();
    if (!needs_prepare) return;
    if (!exit_sent)
    {
        gui_app_exit(job_id);
        gui_app_exec_now();
        if (!prepared_resources) app_service_stop(job_id);
        exit_sent = true;
    }
    rt_list_t *cursor = NULL;
    gui_runing_app_t *app;
    bool alive = false;
    while ((app = gui_app_trav(&cursor)) != NULL)
        if (!strcmp(app->id, job_id)) alive = true;
    alive = alive || (!prepared_resources && (app_service_running(job_id) || dlmodule_find(job_id)));
    if (alive && (rt_tick_t)(rt_tick_get() - prepare_started) < rt_tick_from_millisecond(35000)) return;
    prepare_ok = !alive;
    needs_prepare = false;
    rt_sem_release(&prepared);
}

static void worker(void *parameter)
{
    (void)parameter;
    const install_paths_t *p = &paths[job_volume];
    char source[96], destination[96];
    const char *message = "操作失败，请重试";
    bool ok = false;
    void *buffer = NULL;
    rt_sem_take(&prepared, RT_WAITING_FOREVER);
    if (!prepare_ok) { message = "应用仍在使用，请重试"; goto finish_unlocked; }
    storage_lock();
    if (!storage_available(p->volume) ||
        ((!remove_job || p->volume == STORAGE_SD) && !storage_session_valid(STORAGE_SD, job_session)))
    { message = "TF 卡已移除或更换"; goto finish; }
    if (!volume_ready_set(p->volume, recover_locked(p, NULL)))
    { message = "应用事务恢复失败"; goto finish; }
    snprintf(destination, sizeof(destination), "%s/%s", p->directory, job_id);
    transaction_t transaction = {0};
    strcpy(transaction.id, job_id);
    if (remove_job)
    {
        epd_app_entry_t entry;
        if (!app_package_read(p->directory, job_id, &entry)) goto finish;
        if (!save_transaction(p, &transaction, TX_REMOVE)) goto finish;
        ok = volume_ready_set(p->volume, recover_locked(p, NULL));
        message = ok ? "卸载成功" : "卸载未完成，请重试";
        goto finish;
    }
    if (!storage_available(STORAGE_SD)) { message = "请插入 TF 卡"; goto finish; }
    epd_app_entry_t entry;
    if (!app_package_read(EPD_PACKAGE_DIRECTORY, job_id, &entry))
    { message = "应用包格式不正确"; goto finish; }
    snprintf(source, sizeof(source), EPD_PACKAGE_DIRECTORY "/%s", job_id);
    app_package_status_t package_status = app_package_verify(source, job_id, true);
    if (package_status != APP_PACKAGE_OK)
    {
        rt_kprintf("[apps] install %s: %s\n", job_id, app_package_error(package_status));
        message = package_status == APP_PACKAGE_INCOMPATIBLE ? "应用与当前固件不兼容" : "应用包校验失败";
        goto finish;
    }
    int installed = path_state(destination);
    if (installed < 0) goto finish;
    bool updating = installed != 0;
    if (updating)
    {
        epd_app_entry_t old;
        if (!app_package_metadata(destination, job_id, &old)) goto finish;
        bool weather_migration = !strcmp(job_id, "weather") && old.resources_only &&
                                 !entry.resources_only && old.version == 1 && entry.version >= 2;
        if (old.resources_only != entry.resources_only && !weather_migration)
        { message = "应用包类型不一致"; goto finish; }
        if (entry.version < old.version || (entry.version == old.version && !strcmp(entry.build_id, old.build_id)))
        { message = "已安装相同或更新版本"; goto finish; }
        if (old.data_version && entry.data_version != old.data_version)
        { message = "应用数据版本不兼容"; goto finish; }
    }
    struct statfs fs;
    if (dfs_statfs(storage_root(p->volume), &fs) != 0 || !fs.f_bsize) goto finish;
    uint64_t required = 0;
    if (!tree_walk(source, NULL, 0, TREE_SIZE, fs.f_bsize, &required, NULL))
    { message = "无法读取完整应用包"; goto finish; }
    if (required + (uint64_t)fs.f_bsize * 4 > (uint64_t)fs.f_bfree * fs.f_bsize)
    { message = p->volume == STORAGE_SD ? "TF 卡空间不足" : "内部存储空间不足"; goto finish; }
    buffer = rt_malloc(COPY_SIZE);
    if (!buffer) { message = "内存不足"; goto finish; }
    if (path_state(p->root) == 0 && mkdir(p->root, 0) != 0) goto finish;
    if (path_state(p->directory) == 0 && mkdir(p->directory, 0) != 0) goto finish;
    if (!save_transaction(p, &transaction, TX_COPY)) goto finish;
    uint64_t copied = 0;
    bool staged = tree_walk(source, p->staging, 0, TREE_COPY, fs.f_bsize, &copied, buffer) &&
                  copied == required && storage_available(STORAGE_SD) &&
                  app_package_verify(p->staging, job_id, true) == APP_PACKAGE_OK;
    if (staged && save_transaction(p, &transaction, TX_PUBLISH))
    {
        bool ready = volume_ready_set(p->volume, recover_locked(p, &ok));
        ok = ok && ready;
    }
    else volume_ready_set(p->volume, recover_locked(p, NULL)); /* TX_COPY: discard only staging. */
    if (ok && !migrate_data(job_id, p->volume))
    {
        volume_ready_set(p->volume, false);
        ok = false;
        message = "应用已安装，数据迁移未完成";
    }
    else message = ok ? (updating ? "更新成功" : "安装成功") : "安装失败，请重试";
finish:
    rt_free(buffer);
    storage_unlock();
finish_unlocked:
    /* Also restart a previously stopped background service after a rejected update. */
    app_catalog_changed();
    rt_base_t level = rt_hw_interrupt_disable();
    snprintf(result.message, sizeof(result.message), "%s", message);
    result.state = ok ? APP_JOB_DONE : APP_JOB_FAILED;
    rt_hw_interrupt_enable(level);
}

bool app_installer_start(const char *id, bool uninstall, storage_volume_t volume, uint32_t *ticket)
{
    if (app_installer_busy() || !id || !ticket || !valid_id(id) || (unsigned)volume >= STORAGE_COUNT ||
        !app_installer_volume_ready(volume) || storage_release_requested() || storage_changing()) return false;
    epd_app_entry_t entry;
    storage_lock();
    storage_volume_t existing;
    bool installed = storage_app_locate(id, &existing);
    /* One app ID has one active installation; updating preserves its volume. */
    bool found = (uninstall || !installed || existing == volume) &&
                 app_package_read(uninstall ? storage_app_directory(volume) : EPD_PACKAGE_DIRECTORY, id, &entry);
    storage_unlock();
    if (!found) return false;
    rt_thread_t thread = rt_thread_create("appcopy", worker, NULL, 12288, 23, 10);
    if (!thread) return false;
    snprintf(job_id, sizeof(job_id), "%s", id);
    remove_job = uninstall;
    job_volume = volume;
    job_session = storage_card_session();
    prepared_resources = entry.resources_only;
    exit_sent = prepare_ok = false;
    prepare_started = rt_tick_get();
    needs_prepare = true;
    rt_base_t level = rt_hw_interrupt_disable();
    result.state = APP_JOB_RUNNING;
    result.message[0] = 0;
    *ticket = ++result.ticket;
    rt_hw_interrupt_enable(level);
    if (rt_thread_startup(thread) == RT_EOK) return true;
    needs_prepare = false;
    result.state = APP_JOB_FAILED;
    rt_thread_delete(thread);
    return false;
}
