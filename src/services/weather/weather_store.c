/* SPDX-License-Identifier: Apache-2.0 */
#include "weather_store.h"
#include "mem_map.h"
#include "drv_flash.h"
#include <dfs_posix.h>
#include <dfs_fs.h>
#include <cJSON.h>
#include <string.h>
#include <stddef.h>
#include <ctype.h>

#define RECORD_MAGIC 0x57583031u
#define RECORD_VERSION 1u
#define SLOT_SIZE 4096u
#define SLOT_COUNT (WEATHER_STORE_SIZE / SLOT_SIZE)

typedef struct
{
    uint32_t magic, crc, version, sequence;
    weather_config_t config;
    weather_info_t info;
} weather_record_t;

typedef char record_fits_slot[(sizeof(weather_record_t) <= SLOT_SIZE) ? 1 : -1];
static uint32_t sequence;
static unsigned next_slot;

static uint32_t record_crc(const weather_record_t *record)
{
    const uint8_t *p = (const uint8_t *)&record->version;
    size_t size = sizeof(*record) - offsetof(weather_record_t, version);
    uint32_t crc = UINT32_MAX;
    while (size--)
    {
        crc ^= *p++;
        for (unsigned i = 0; i < 8; ++i)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

bool weather_store_load(weather_config_t *config, weather_info_t *info)
{
    weather_record_t record;
    bool found = false;
    for (unsigned i = 0; i < SLOT_COUNT; ++i)
    {
        uint32_t address = WEATHER_STORE_START_ADDR + i * SLOT_SIZE;
        if (rt_flash_read(address, (uint8_t *)&record, sizeof(record)) != sizeof(record) ||
            record.magic != RECORD_MAGIC || record.version != RECORD_VERSION ||
            record.crc != record_crc(&record)) continue;
        if (found && (int32_t)(record.sequence - sequence) <= 0) continue;
        sequence = record.sequence;
        next_slot = (i + 1) % SLOT_COUNT;
        *config = record.config;
        *info = record.info;
        found = true;
    }
    return found;
}

bool weather_store_save(const weather_config_t *config, const weather_info_t *info)
{
    weather_record_t record, check;
    uint32_t address = WEATHER_STORE_START_ADDR + next_slot * SLOT_SIZE;
    memset(&record, 0, sizeof(record));
    record.magic = RECORD_MAGIC;
    record.version = RECORD_VERSION;
    record.sequence = sequence + 1;
    record.config = *config;
    record.info = *info;
    record.crc = record_crc(&record);
    /* Rotate sectors. Commit magic last; an interrupted write retains the old slot. */
    if (rt_flash_erase(address, SLOT_SIZE) != RT_EOK ||
        rt_flash_write(address + 4, (const uint8_t *)&record + 4, sizeof(record) - 4) != sizeof(record) - 4 ||
        rt_flash_read(address + 4, (uint8_t *)&check + 4, sizeof(check) - 4) != sizeof(check) - 4 ||
        memcmp((const uint8_t *)&record + 4, (const uint8_t *)&check + 4, sizeof(record) - 4) ||
        rt_flash_write(address, (const uint8_t *)&record.magic, 4) != 4 ||
        rt_flash_read(address, (uint8_t *)&check.magic, 4) != 4 || check.magic != RECORD_MAGIC)
        return false;
    sequence = record.sequence;
    next_slot = (next_slot + 1) % SLOT_COUNT;
    return true;
}

bool weather_config_valid(const weather_config_t *config)
{
    static const char suffix[] = ".qweatherapi.com";
    size_t host_len = strlen(config->api_host), key_len = strlen(config->api_key);
    if (host_len <= sizeof(suffix) - 1 || host_len >= sizeof(config->api_host) ||
        strcmp(config->api_host + host_len - (sizeof(suffix) - 1), suffix) ||
        key_len == 0 || key_len >= sizeof(config->api_key)) return false;
    for (size_t i = 0; i < host_len; ++i)
        if (!isalnum((unsigned char)config->api_host[i]) && config->api_host[i] != '-' &&
            config->api_host[i] != '.') return false;
    for (size_t i = 0; i < key_len; ++i)
        if (!isalnum((unsigned char)config->api_key[i]) && config->api_key[i] != '-' &&
            config->api_key[i] != '_') return false;
    return true;
}

bool weather_store_import(weather_config_t *config, char *error, size_t size)
{
    rt_device_t sd = rt_device_find("sd0");
    const char *mount;
    char path[128], json[1025];
    int fd = -1, count;
    struct stat st;
    cJSON *root = RT_NULL, *host, *key;
    bool ok = false;
    rt_snprintf(error, size, "请插入 TF 卡");
    if (!sd) sd = rt_device_find("sd1");
    if (!sd) return false;
    mount = dfs_filesystem_get_mounted_path(sd);
    if (!mount)
    {
        mkdir("/sdcard", 0);
        if (dfs_mount(sd->parent.name, "/sdcard", "elm", 0, RT_NULL) != 0) return false;
        mount = "/sdcard";
    }
    rt_snprintf(path, sizeof(path), "%s%s%s", mount, strcmp(mount, "/") ? "/" : "", WEATHER_CONFIG_FILE);
    rt_snprintf(error, size, "未找到 qweather.json");
    fd = open(path, O_RDONLY, 0);
    if (fd < 0) return false;
    rt_snprintf(error, size, "配置文件格式错误");
    if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > 1024) goto finish;
    count = read(fd, json, st.st_size);
    if (count != st.st_size) goto finish;
    json[count] = '\0';
    const char *start = json;
    if (count >= 3 && (unsigned char)json[0] == 0xef &&
        (unsigned char)json[1] == 0xbb && (unsigned char)json[2] == 0xbf) start += 3;
    root = cJSON_ParseWithOpts(start, RT_NULL, 1);
    if (!cJSON_IsObject(root) || cJSON_GetArraySize(root) != 2) goto finish;
    host = cJSON_GetObjectItemCaseSensitive(root, "api_host");
    key = cJSON_GetObjectItemCaseSensitive(root, "api_key");
    if (!cJSON_IsString(host) || !cJSON_IsString(key) ||
        strlen(host->valuestring) >= sizeof(config->api_host) ||
        strlen(key->valuestring) >= sizeof(config->api_key)) goto finish;
    const char *host_text = host->valuestring;
    if (!strncmp(host_text, "https://", 8)) host_text += 8;
    rt_snprintf(config->api_host, sizeof(config->api_host), "%s", host_text);
    size_t n = strlen(config->api_host);
    if (n && config->api_host[n - 1] == '/') config->api_host[n - 1] = '\0';
    rt_snprintf(config->api_key, sizeof(config->api_key), "%s", key->valuestring);
    ok = weather_config_valid(config);
finish:
    if (root) cJSON_Delete(root);
    close(fd);
    memset(json, 0, sizeof(json));
    return ok;
}
