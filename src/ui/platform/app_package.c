#include "app_package.h"
#include "epd_app.h"
#include "epd_app_profile.h"
#include "storage.h"
#include "icons/ui_icons.h"
#include <cJSON.h>
#include <dfs_posix.h>
#include <mbedtls/sha256.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

static bool path_join(char *path, size_t size, const char *root, const char *name)
{
    int n = snprintf(path, size, "%s/%s", root, name);
    return n > 0 && (size_t)n < size;
}

static bool relative_path(const char *path)
{
    if (!path || !*path || strlen(path) >= EPD_PACKAGE_PATH_MAX) return false;
    const char *part = path;
    unsigned depth = 1;
    for (const char *p = path; ; ++p)
    {
        if (*p && ((unsigned char)*p < 32 || *p == '\\' || *p == ':')) return false;
        if (*p == '/' || !*p)
        {
            size_t length = p - part;
            if (!length || (length == 1 && part[0] == '.') ||
                (length == 2 && part[0] == '.' && part[1] == '.')) return false;
            if (!*p) return true;
            if (++depth > 8) return false;
            part = p + 1;
        }
    }
}

static cJSON *manifest_read(const char *root)
{
    char path[256];
    if (!path_join(path, sizeof(path), root, "app.json")) return NULL;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        (uint64_t)st.st_size >= SIZE_MAX)
    { close(fd); return NULL; }
    size_t size = (size_t)st.st_size, length = 0;
    char *text = epd_app_alloc(size + 1, EPD_APP_PSRAM);
    if (!text) { close(fd); return NULL; }
    while (length < size)
    {
        size_t amount = size - length;
        if (amount > 4096) amount = 4096;
        int n = read(fd, text + length, amount);
        if (n <= 0) break;
        length += n;
    }
    close(fd);
    text[length] = 0;
    cJSON *json = length == size ? cJSON_ParseWithOpts(text, NULL, true) : NULL;
    epd_app_free(text);
    return json;
}

static const char *string_value(const cJSON *json, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static bool positive_integer(const cJSON *json, const char *key, uint32_t *value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    if (!cJSON_IsNumber(item) || !(item->valuedouble >= 1 && item->valuedouble <= UINT32_MAX)) return false;
    uint32_t number = (uint32_t)item->valuedouble;
    if (number != item->valuedouble) return false;
    if (value) *value = number;
    return true;
}

bool app_package_metadata(const char *root, const char *id, epd_app_entry_t *entry)
{
    cJSON *json = manifest_read(root);
    if (!json) return false;
    const char *name = string_value(json, "name");
    const char *kind = string_value(json, "kind");
    bool resources_only = kind && !strcmp(kind, "resources");
    bool ok = name && *name && strlen(name) < sizeof(entry->name);
    char path[256], program[RT_NAME_MAX + 4];
    snprintf(program, sizeof(program), "%s.so", id);
    struct stat st;
    ok = ok && (!kind || !strcmp(kind, "module") || resources_only) &&
         (resources_only || (path_join(path, sizeof(path), root, program) &&
         stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 52));
    if (ok)
    {
        memset(entry, 0, sizeof(*entry));
        snprintf(entry->id, sizeof(entry->id), "%s", id);
        snprintf(entry->name, sizeof(entry->name), "%s", name);
        entry->icon = &ui_icon_album;
        const char *icon = string_value(json, "icon");
        if (icon && relative_path(icon))
            snprintf(entry->icon_file, sizeof(entry->icon_file), "%s", icon);
        entry->has_settings = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "settings"));
        entry->resources_only = resources_only;
        entry->background = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "background"));
        positive_integer(json, "version", &entry->version);
        positive_integer(json, "data_version", &entry->data_version);
        const char *build = string_value(json, "build_id");
        if (build) snprintf(entry->build_id, sizeof(entry->build_id), "%s", build);
    }
    cJSON_Delete(json);
    return ok;
}

static bool hash_text(const char *text)
{
    if (!text || strlen(text) != 64) return false;
    for (unsigned i = 0; i < 64; ++i)
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) return false;
    return true;
}

static bool file_hash(const char *path, const char *expected)
{
    unsigned char buffer[1024], result[32];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    struct stat st;
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
    mbedtls_sha256_context hash;
    mbedtls_sha256_init(&hash);
    ok = ok && mbedtls_sha256_starts_ret(&hash, 0) == 0;
    int n = 0;
    while (ok && storage_path_available(path) && (n = read(fd, buffer, sizeof(buffer))) > 0)
        ok = mbedtls_sha256_update_ret(&hash, buffer, n) == 0;
    ok = ok && n == 0 && storage_path_available(path) && mbedtls_sha256_finish_ret(&hash, result) == 0;
    mbedtls_sha256_free(&hash);
    close(fd);
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; ok && i < sizeof(result); ++i)
        ok = expected[i * 2] == hex[result[i] >> 4] && expected[i * 2 + 1] == hex[result[i] & 15];
    return ok;
}

static uint32_t get_u32(const unsigned char *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static unsigned get_u16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }

static bool elf_valid(const char *path, uint32_t expected_span, size_t *load_peak)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    unsigned char header[52], segment[32];
    struct stat st;
    bool ok = fstat(fd, &st) == 0 && st.st_size >= 52 && read(fd, header, sizeof(header)) == sizeof(header);
    ok = ok && !memcmp(header, "\177ELF\1\1\1", 7) && get_u16(header + 16) == 3 &&
         get_u16(header + 18) == 40 && get_u16(header + 42) == sizeof(segment);
    uint32_t start = 0, end = 0;
    bool found = false;
    if (ok)
    {
        uint32_t phoff = get_u32(header + 28);
        unsigned count = get_u16(header + 44);
        ok = (uint64_t)phoff + (uint64_t)count * sizeof(segment) <= st.st_size &&
             lseek(fd, phoff, SEEK_SET) == phoff;
        for (unsigned i = 0; ok && i < count; ++i)
        {
            ok = read(fd, segment, sizeof(segment)) == sizeof(segment);
            if (!ok || get_u32(segment) != 1) continue;
            uint32_t offset = get_u32(segment + 4), address = get_u32(segment + 8);
            uint32_t file_size = get_u32(segment + 16), memory_size = get_u32(segment + 20);
            ok = file_size <= memory_size && (uint64_t)offset + file_size <= st.st_size &&
                 (uint64_t)address + memory_size <= UINT32_MAX && (!found || address >= end);
            if (!found) start = address;
            end = address + memory_size;
            found = true;
        }
    }
    close(fd);
    if (!ok || !found || end <= start || end - start != expected_span) return false;
    if (load_peak)
    {
        /* Cache reclamation hint, not an allocation admission check. */
        uint64_t bytes = (uint64_t)st.st_size + expected_span;
        *load_peak = bytes > SIZE_MAX ? SIZE_MAX : (size_t)bytes;
    }
    return true;
}

static bool file_count(const char *root, unsigned depth, uint64_t *count)
{
    if (depth > 8) return false;
    DIR *dir = opendir(root);
    if (!dir) return false;
    bool ok = true;
    struct dirent *entry;
    while (ok && (entry = readdir(dir)) != NULL)
    {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char path[256];
        struct stat st;
        ok = path_join(path, sizeof(path), root, entry->d_name) && stat(path, &st) == 0;
        if (!ok) break;
        if (S_ISDIR(st.st_mode)) ok = file_count(path, depth + 1, count);
        else if (S_ISREG(st.st_mode))
        {
            ok = *count != UINT64_MAX;
            if (ok) ++*count;
        }
        else ok = false;
    }
    closedir(dir);
    return ok;
}

static bool library_name(const char *name)
{
    if (!name || !*name || strlen(name) >= RT_NAME_MAX) return false;
    for (const char *p = name; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return false;
    return true;
}

static app_package_status_t package_verify(const char *root, const char *id, bool all_files,
                                           const char *requested, size_t *load_peak)
{
    cJSON *json = manifest_read(root);
    if (!json) return APP_PACKAGE_FORMAT_ERROR;
    app_package_status_t status = APP_PACKAGE_FORMAT_ERROR;
    uint32_t format, abi, load_bytes = 0;
    const char *kind = string_value(json, "kind");
    bool resources_only = kind && !strcmp(kind, "resources");
    const char *declared_id = string_value(json, "id");
    const char *build = string_value(json, "build_id");
    const char *digest = string_value(json, "files_sha256");
    const char *icon = string_value(json, "icon");
    const char *name = string_value(json, "name");
    const cJSON *icon_item = cJSON_GetObjectItemCaseSensitive(json, "icon");
    const cJSON *libraries = cJSON_GetObjectItemCaseSensitive(json, "libraries");
    unsigned library_count = 0, library_seen = 0;
    if (requested && !library_name(requested)) goto finish;
    if (libraries)
    {
        if (!cJSON_IsObject(libraries) || resources_only) goto finish;
        const cJSON *item;
        cJSON_ArrayForEach(item, libraries)
        {
            if (library_count == UINT_MAX || !library_name(item->string) ||
                !strcmp(item->string, id) || !positive_integer(libraries, item->string, NULL)) goto finish;
            ++library_count;
            for (const cJSON *previous = libraries->child; previous != item; previous = previous->next)
                if (!strcmp(previous->string, item->string)) goto finish;
        }
    }
    if (requested && !positive_integer(libraries, requested, NULL)) goto finish;
    if (!positive_integer(json, "package_format", &format) || format != EPD_PACKAGE_FORMAT ||
        !positive_integer(json, "abi", &abi) || !positive_integer(json, "version", NULL) ||
        !positive_integer(json, "data_version", NULL) ||
        (!resources_only && !positive_integer(json, "load_bytes", &load_bytes)) ||
        (kind && strcmp(kind, "module") && !resources_only) ||
        !name || !*name || strlen(name) >= 48 || !declared_id || strcmp(declared_id, id) ||
        !hash_text(build) || !hash_text(digest) || (icon_item && !cJSON_IsString(icon_item)) ||
        !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(json, "settings")) ||
        !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(json, "background")) ||
        (icon && !relative_path(icon)) ||
        (resources_only && (!icon || !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(json, "settings")) ||
                             !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(json, "background"))))) goto finish;
    status = APP_PACKAGE_INCOMPATIBLE;
    if (abi != EPD_APP_ABI || strcmp(build, EPD_APP_BUILD_ID)) goto finish;
    status = APP_PACKAGE_INTEGRITY_ERROR;
    char path[256], program[RT_NAME_MAX + 4];
    if (!path_join(path, sizeof(path), root, "files.sha256") || !file_hash(path, digest)) goto finish;
    FILE *index = fopen(path, "rb");
    if (!index) { status = APP_PACKAGE_IO_ERROR; goto finish; }
    char line[64 + 2 + EPD_PACKAGE_PATH_MAX + 2], previous[EPD_PACKAGE_PATH_MAX] = {0};
    uint64_t count = 0;
    bool ok = true, has_program = resources_only, has_icon = !icon;
    snprintf(program, sizeof(program), "%s.so", id);
    while (ok && fgets(line, sizeof(line), index))
    {
        size_t length = strlen(line);
        if (length < 68 || line[length - 1] != '\n' || line[64] != ' ' || line[65] != ' ')
        { ok = false; break; }
        line[length - 1] = 0;
        line[64] = 0;
        const char *name = line + 66;
        ok = count < UINT64_MAX - 2 && hash_text(line) && relative_path(name) &&
             strcmp(previous, name) < 0 && strcmp(name, "app.json") && strcmp(name, "files.sha256") &&
             path_join(path, sizeof(path), root, name);
        if (!ok) break;
        ++count;
        snprintf(previous, sizeof(previous), "%s", name);
        bool is_program = !strcmp(name, program);
        const char *selected_library = NULL;
        uint32_t library_bytes = 0;
        const cJSON *item;
        cJSON_ArrayForEach(item, libraries)
        {
            char filename[32];
            snprintf(filename, sizeof(filename), "codecs/%s.so", item->string);
            if (!strcmp(name, filename))
            {
                selected_library = item->string;
                library_bytes = (uint32_t)item->valuedouble;
                /* Unique manifest names and sorted unique file paths ensure
                 * that each declared library contributes exactly once. */
                ++library_seen;
                break;
            }
        }
        if (resources_only && strcmp(name, icon) && strncmp(name, "res/", 4))
        { ok = false; break; }
        bool check_library = selected_library && (all_files ||
                             (requested && !strcmp(requested, selected_library)));
        if (all_files || is_program || check_library) ok = file_hash(path, line);
        if (is_program) { has_program = true; ok = !resources_only && ok && elf_valid(path, load_bytes, NULL); }
        if (check_library) ok = ok && elf_valid(path, library_bytes,
                              requested && !strcmp(requested, selected_library) ? load_peak : NULL);
        if (icon && !strcmp(name, icon)) has_icon = true;
    }
    ok = ok && !ferror(index) && has_program && has_icon &&
         library_seen == library_count;
    fclose(index);
    if (ok && all_files)
    {
        uint64_t actual = 0;
        ok = file_count(root, 0, &actual) && actual == count + 2;
    }
    if (ok) status = APP_PACKAGE_OK;
finish:
    cJSON_Delete(json);
    return status;
}

app_package_status_t app_package_verify(const char *root, const char *id, bool all_files)
{
    return package_verify(root, id, all_files, NULL, NULL);
}

app_package_status_t app_package_verify_library(const char *root, const char *id,
                                               const char *name, size_t *load_peak)
{
    if (load_peak) *load_peak = 0;
    return package_verify(root, id, false, name, load_peak);
}

const char *app_package_error(app_package_status_t status)
{
    switch (status)
    {
    case APP_PACKAGE_OK: return "ok";
    case APP_PACKAGE_FORMAT_ERROR: return "manifest format";
    case APP_PACKAGE_INCOMPATIBLE: return "firmware profile";
    case APP_PACKAGE_IO_ERROR: return "file access";
    default: return "file integrity or ELF layout";
    }
}
