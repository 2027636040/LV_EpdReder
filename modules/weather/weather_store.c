/* SPDX-License-Identifier: Apache-2.0 */
#include "weather_store.h"
#include "storage.h"
#include "storage_file.h"
#include <dfs_posix.h>
#include <dfs_fs.h>
#include <cJSON.h>
#include <string.h>
#include <stddef.h>
#include <ctype.h>

#define CONFIG_MAGIC 0x57584332u
#define CACHE_MAGIC 0x57584932u
#define RECORD_VERSION 1u
static char config_path[96], cache_path[96];

typedef struct
{
    uint32_t config_crc;
    weather_info_t info;
} weather_cache_t;

static weather_config_t saved_config;
static bool saved_valid;

bool weather_store_load(weather_config_t *config, weather_info_t *info)
{
    if (!storage_app_path(config_path, sizeof(config_path), "weather", STORAGE_APP_DATA, "config.bin") ||
        !storage_app_path(cache_path, sizeof(cache_path), "weather", STORAGE_APP_CACHE, "current.bin")) return false;
    weather_config_t candidate;
    weather_cache_t cache;
    saved_valid = storage_record_load(config_path, CONFIG_MAGIC, RECORD_VERSION,
                                      &candidate, sizeof(candidate));
    if (!saved_valid) return false;
    *config = saved_config = candidate;
    /* Cache is dispensable. Never combine a city's old snapshot with new config. */
    if (storage_record_load(cache_path, CACHE_MAGIC, RECORD_VERSION, &cache, sizeof(cache)) &&
        cache.config_crc == storage_crc32(config, sizeof(*config)))
    {
        *info = cache.info;
    }
    return true;
}

bool weather_store_save(const weather_config_t *config, const weather_info_t *info)
{
    if (!storage_app_available("weather")) return false;
    weather_cache_t cache = {0};
    cache.config_crc = storage_crc32(config, sizeof(*config));
    cache.info = *info;
    if (!storage_record_save(cache_path, CACHE_MAGIC, RECORD_VERSION, &cache, sizeof(cache))) return false;
    /* Publish config last. An interrupted pair can invalidate cache, not config. */
    if ((!saved_valid || memcmp(config, &saved_config, sizeof(*config))) &&
        !storage_record_save(config_path, CONFIG_MAGIC, RECORD_VERSION, config, sizeof(*config))) return false;
    saved_config = *config;
    saved_valid = true;
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
    char path[128], json[1025];
    int fd = -1, count;
    struct stat st;
    cJSON *root = RT_NULL, *host, *key;
    bool ok = false;
    rt_snprintf(error, size, "请插入 TF 卡");
    storage_lock();
    if (!storage_available(STORAGE_SD)) goto finish;
    rt_snprintf(path, sizeof(path), STORAGE_SD_ROOT "/%s", WEATHER_CONFIG_FILE);
    rt_snprintf(error, size, "未找到 qweather.json");
    fd = open(path, O_RDONLY, 0);
    if (fd < 0) goto finish;
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
    if (fd >= 0) close(fd);
    memset(json, 0, sizeof(json));
    storage_unlock();
    return ok;
}
