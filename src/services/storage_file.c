#include "storage_file.h"
#include <dfs_posix.h>
#include <string.h>
#include <rtm.h>
#include <stdio.h>
#include <errno.h>
#include "platform/app_module.h"

typedef struct { uint32_t magic, version, size, crc; } record_header_t;

static bool managed_path(const char *path)
{
    return path && (!strncmp(path, STORAGE_FLASH_ROOT "/", sizeof(STORAGE_FLASH_ROOT)) ||
                    !strncmp(path, STORAGE_SD_ROOT "/", sizeof(STORAGE_SD_ROOT)));
}

const char *storage_app_directory(storage_volume_t volume)
{
    return volume == STORAGE_SD ? STORAGE_SD_APPS : STORAGE_FLASH_APPS;
}

static bool app_id_valid(const char *id)
{
    if (!id || !*id || strlen(id) >= RT_NAME_MAX) return false;
    for (const char *p = id; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return false;
    return true;
}

bool storage_app_locate(const char *id, storage_volume_t *volume)
{
    if (!app_id_valid(id) || !volume) return false;
    char path[96];
    struct stat st;
    bool found = false;
    storage_lock();
    /* A duplicate copied in by a PC never replaces the internal installation. */
    for (storage_volume_t v = STORAGE_FLASH; v < STORAGE_COUNT; ++v)
    {
        rt_snprintf(path, sizeof(path), "%s/%s/app.json", storage_app_directory(v), id);
        if (storage_available(v) && stat(path, &st) == 0 && S_ISREG(st.st_mode))
        { *volume = v; found = true; break; }
    }
    storage_unlock();
    return found;
}

static bool app_location(const char *id, storage_volume_t *volume)
{
    uint32_t session;
    if (app_module_storage(id, volume, &session)) return storage_session_valid(*volume, session);
    return storage_app_locate(id, volume);
}

bool storage_app_available(const char *id)
{
    storage_volume_t volume;
    return app_location(id, &volume);
}
RTM_EXPORT(storage_app_available);

bool storage_app_path(char *out, size_t size, const char *id,
                      storage_app_area_t area, const char *relative)
{
    static const char *const areas[] = {"apps", "data", "cache"};
    if (out && size) out[0] = 0;
    if (!out || !size || !app_id_valid(id) || (unsigned)area >= 3 || !relative) return false;
    const char *segment = relative;
    for (const char *p = relative; ; ++p)
    {
        if (*p && ((unsigned char)*p < 32 || *p == '\\' || *p == ':')) return false;
        if (*p == '/' || !*p)
        {
            size_t n = p - segment;
            if ((!n && *relative) || (n == 1 && segment[0] == '.') ||
                (n == 2 && segment[0] == '.' && segment[1] == '.')) return false;
            segment = p + 1;
        }
        if (!*p) break;
    }
    int n;
    storage_volume_t volume;
    if (!app_location(id, &volume)) return false;
    if (area == STORAGE_APP_CODE)
    {
        n = snprintf(out, size, "%s/%s%s%s", storage_app_directory(volume), id,
                        *relative ? "/" : "", relative);
    }
    else
        n = snprintf(out, size, "%s/%s/%s%s%s",
                        volume == STORAGE_SD ? STORAGE_SD_ROOT "/.epd" : STORAGE_FLASH_ROOT, areas[area], id,
                        *relative ? "/" : "", relative);
    if (n <= 0 || (size_t)n >= size) { out[0] = 0; return false; }
    return true;
}
RTM_EXPORT(storage_app_path);

bool storage_mkdirs(const char *path)
{
    char directory[STORAGE_PATH_MAX];
    struct stat st;
    if (!path || !*path) return false;
    const char *root = storage_path_root(path);
    if (!strcmp(path, root)) return storage_path_available(path);
    if (!managed_path(path) || strlen(path) >= sizeof(directory)) return false;
    strcpy(directory, path);
    bool ok = true;
    storage_lock();
    if (!storage_path_available(path)) ok = false;
    for (char *p = directory + strlen(root) + 1; ok; ++p)
    {
        if (*p != '/' && *p) continue;
        char separator = *p;
        if (!storage_path_available(path)) { ok = false; break; }
        *p = 0;
        if (stat(directory, &st) == 0) ok = S_ISDIR(st.st_mode);
        else ok = mkdir(directory, 0) == 0;
        *p = separator;
        if (!separator) break;
    }
    storage_unlock();
    return ok;
}
RTM_EXPORT(storage_mkdirs);

void storage_prepare_directories(void)
{
    static const char *const dirs[] = {"apps", "data", "cache", "system", "fonts", "update"};
    char path[32];
    for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i)
    {
        rt_snprintf(path, sizeof(path), STORAGE_FLASH_ROOT "/%s", dirs[i]);
        if (!storage_mkdirs(path)) rt_kprintf("[storage] mkdir failed: %s\n", path);
    }
}

uint32_t storage_crc32(const void *data, size_t size)
{
    const uint8_t *p = data;
    uint32_t crc = UINT32_MAX;
    while (size--)
    {
        crc ^= *p++;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
RTM_EXPORT(storage_crc32);

static bool transfer_write(int fd, const void *data, size_t size)
{
    const uint8_t *p = data;
    while (size)
    {
        int n = write(fd, p, size);
        if (n <= 0) return false;
        p += n;
        size -= n;
    }
    return true;
}

static int replace_begin(const char *path, char temporary[STORAGE_PATH_MAX])
{
    if (!managed_path(path) || strlen(path) + 5 >= STORAGE_PATH_MAX ||
        !storage_path_available(path) || !storage_file_recover(path)) return -1;
    strcpy(temporary, path);
    char *slash = strrchr(temporary, '/');
    *slash = 0;
    if (!storage_mkdirs(temporary)) return -1;
    rt_snprintf(temporary, STORAGE_PATH_MAX, "%s.tmp", path);
    return open(temporary, O_CREAT | O_TRUNC | O_WRONLY, 0);
}

static int file_state(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) return 1;
    return errno == -ENOENT ? 0 : -1;
}

bool storage_file_recover(const char *path)
{
    if (!managed_path(path) || strlen(path) + 5 >= STORAGE_PATH_MAX) return false;
    storage_lock();
    bool ok = storage_path_available(path);
    if (ok && !strcmp(storage_path_root(path), STORAGE_SD_ROOT))
    {
        char backup[STORAGE_PATH_MAX];
        snprintf(backup, sizeof(backup), "%s.old", path);
        int saved = file_state(backup), current = file_state(path);
        ok = saved >= 0 && current >= 0;
        if (ok && saved && !current) ok = rename(backup, path) == 0;
    }
    storage_unlock();
    return ok;
}
RTM_EXPORT(storage_file_recover);

bool storage_file_commit(const char *temporary, const char *path)
{
    if (!managed_path(temporary) || !managed_path(path) || !strcmp(temporary, path) ||
        strlen(path) + 5 >= STORAGE_PATH_MAX ||
        strcmp(storage_path_root(temporary), storage_path_root(path))) return false;
    storage_lock();
    bool ok = storage_path_available(path);
    bool card = !strcmp(storage_path_root(path), STORAGE_SD_ROOT);
    char backup[STORAGE_PATH_MAX];
    snprintf(backup, sizeof(backup), "%s.old", path);
    if (ok && card)
    {
        ok = storage_file_recover(path);
        int saved = ok ? file_state(backup) : -1;
        int current = ok ? file_state(path) : -1;
        ok = saved >= 0 && current >= 0;
        if (ok && saved) ok = unlink(backup) == 0;
        if (ok && current) ok = rename(path, backup) == 0;
    }
    if (ok) ok = storage_path_available(path) && rename(temporary, path) == 0;
    if (card && storage_path_available(path))
    {
        if (ok) unlink(backup);
        else storage_file_recover(path);
    }
    storage_unlock();
    return ok;
}
RTM_EXPORT(storage_file_commit);

static bool replace_finish(int fd, const char *temporary, const char *path, bool ok)
{
    if (ok) ok = storage_path_available(path) && fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    if (ok) ok = storage_file_commit(temporary, path);
    if (!ok && storage_path_available(path)) unlink(temporary);
    return ok;
}

bool storage_file_replace(const char *path, const void *data, size_t size)
{
    char temporary[STORAGE_PATH_MAX];
    storage_lock();
    int fd = replace_begin(path, temporary);
    bool ok = fd >= 0 && replace_finish(fd, temporary, path, transfer_write(fd, data, size));
    storage_unlock();
    return ok;
}
RTM_EXPORT(storage_file_replace);

bool storage_record_save(const char *path, uint32_t magic, uint32_t version, const void *data, size_t size)
{
    record_header_t header = {magic, version, size, storage_crc32(data, size)};
    char temporary[STORAGE_PATH_MAX];
    storage_lock();
    int fd = replace_begin(path, temporary);
    bool ok = fd >= 0 && replace_finish(fd, temporary, path,
                    transfer_write(fd, &header, sizeof(header)) && transfer_write(fd, data, size));
    storage_unlock();
    return ok;
}
RTM_EXPORT(storage_record_save);

bool storage_record_load(const char *path, uint32_t magic, uint32_t version, void *data, size_t size)
{
    record_header_t header;
    struct stat st;
    storage_lock();
    int fd = storage_file_recover(path) ? open(path, O_RDONLY) : -1;
    bool ok = fd >= 0 && fstat(fd, &st) == 0 && st.st_size == sizeof(header) + size &&
              read(fd, &header, sizeof(header)) == sizeof(header) &&
              header.magic == magic && header.version == version && header.size == size &&
              read(fd, data, size) == size && header.crc == storage_crc32(data, size);
    if (fd >= 0) close(fd);
    storage_unlock();
    return ok;
}
RTM_EXPORT(storage_record_load);

bool storage_file_import(const char *source, const char *destination)
{
    char temporary[STORAGE_PATH_MAX];
    struct stat st;
    uint8_t *buffer = rt_malloc(4096);
    if (!buffer) return false;
    storage_lock();
    int input = storage_path_available(source) ? open(source, O_RDONLY) : -1;
    bool ok = input >= 0 && fstat(input, &st) == 0 && S_ISREG(st.st_mode);
    int output = ok ? replace_begin(destination, temporary) : -1;
    ok = output >= 0;
    size_t remaining = ok ? st.st_size : 0;
    while (ok && remaining)
    {
        if (!storage_path_available(source) || !storage_path_available(destination)) { ok = false; break; }
        int n = read(input, buffer, remaining < 4096 ? remaining : 4096);
        if (n <= 0 || !transfer_write(output, buffer, n)) { ok = false; break; }
        remaining -= n;
    }
    if (input >= 0 && close(input) != 0) ok = false;
    if (output >= 0) ok = replace_finish(output, temporary, destination, ok);
    storage_unlock();
    rt_free(buffer);
    return ok;
}

bool storage_file_signature(const char *path, uint32_t *signature)
{
    uint8_t buffer[1024];
    uint32_t hash = 2166136261u;
    storage_lock();
    int fd = storage_path_available(path) ? open(path, O_RDONLY) : -1;
    int n = -1;
    if (fd >= 0)
    {
        while (storage_path_available(path) && (n = read(fd, buffer, sizeof(buffer))) > 0)
            for (int i = 0; i < n; ++i) hash = (hash ^ buffer[i]) * 16777619u;
        if (close(fd) != 0 || !storage_path_available(path)) n = -1;
    }
    storage_unlock();
    if (n != 0) return false;
    *signature = hash;
    return true;
}
RTM_EXPORT(storage_file_import);
RTM_EXPORT(storage_file_signature);
