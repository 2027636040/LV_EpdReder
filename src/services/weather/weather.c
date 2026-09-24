/* SPDX-License-Identifier: Apache-2.0 */
#include "weather.h"
#include "weather_store.h"
#include "weather_http.h"
#include "bt_pan.h"
#include <rthw.h>
#include <cJSON.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>

#define DBG_TAG "weather"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define UPDATE_INTERVAL_MS (30 * 60 * 1000)
#define RETRY_INTERVAL_MS 5000u
typedef struct
{
    weather_job_t job;
    uint32_t ticket;
    char city_id[WEATHER_CITY_ID_MAX];
} weather_request_t;

static rt_mutex_t lock;
static rt_mq_t requests;
static weather_config_t config;
static weather_info_t info;
static weather_result_t result;
static uint32_t next_ticket;
static uint32_t pan_generation;
static bool refresh(const weather_config_t *cfg);
static void publish(const weather_config_t *cfg, weather_info_t *data);

static void pan_state_changed(btpan_state_t state)
{
    if (state != BTPAN_STATE_NETWORK_READY) return;
    /* Record reconnects even while the weather worker is inside an HTTP request. */
    rt_base_t level = rt_hw_interrupt_disable();
    ++pan_generation;
    rt_hw_interrupt_enable(level);
}

static uint32_t pan_generation_get(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    uint32_t generation = pan_generation;
    rt_hw_interrupt_enable(level);
    return generation;
}

static void empty_info(weather_info_t *out, const weather_config_t *cfg)
{
    memset(out, 0, sizeof(*out));
    out->code = 999;
    out->temperature = out->feels_like = out->high = out->low = WEATHER_MISSING;
    out->humidity = out->wind_speed = out->visibility = out->pressure = out->cloud = WEATHER_MISSING;
    out->wind_scale = WEATHER_MISSING;
    out->aqi = -1;
    rt_snprintf(out->city, sizeof(out->city), "%s", cfg->city);
    rt_snprintf(out->city_id, sizeof(out->city_id), "%s", cfg->city_id);
    rt_snprintf(out->region, sizeof(out->region), "%s", cfg->region);
}

static bool digits(const char *text)
{
    if (!text || !text[0] || strlen(text) >= WEATHER_CITY_ID_MAX) return false;
    for (; *text; ++text) if (*text < '0' || *text > '9') return false;
    return true;
}

static bool copy_string(char *dst, size_t capacity, cJSON *object, const char *name)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsString(item) || strlen(item->valuestring) >= capacity) return false;
    rt_snprintf(dst, capacity, "%s", item->valuestring);
    return dst[0] != '\0';
}

static int number(cJSON *object, const char *name, int min, int max)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    double value;
    if (cJSON_IsNumber(item)) value = item->valuedouble;
    else if (cJSON_IsString(item) && item->valuestring[0])
    {
        char *end;
        value = strtod(item->valuestring, &end);
        if (*end) return WEATHER_MISSING;
    }
    else return WEATHER_MISSING;
    if (!isfinite(value) || value < min || value > max) return WEATHER_MISSING;
    return (int)(value >= 0 ? value + 0.5 : value - 0.5);
}

static bool api_ok(cJSON *root)
{
    cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    bool ok = cJSON_IsString(code) && !strcmp(code->valuestring, "200");
    if (!cJSON_IsString(code)) LOG_W("stage=api-code missing or invalid code");
    else if (!ok) LOG_W("stage=api-code QWeather code=%.8s", code->valuestring);
    return ok;
}

static cJSON *get_json(const weather_config_t *cfg, const char *path)
{
    cJSON *root = RT_NULL;
    for (unsigned attempt = 0; attempt < 2; ++attempt)
    {
        if (!weather_network_ready())
        {
            LOG_W("stage=network PAN/IP not ready");
            break;
        }
        int http_status;
        char *json = weather_http_get(cfg, path, &http_status);
        if (json)
        {
            const char *parse_end = RT_NULL;
            size_t bytes = strlen(json);
            root = cJSON_ParseWithOpts(json, &parse_end, 1);
            if (!root)
                LOG_W("stage=json-parse bytes=%u offset=%u", (unsigned)bytes,
                      (unsigned)(parse_end ? parse_end - json : 0));
            else if (!cJSON_IsObject(root))
                LOG_W("stage=json-root expected object; type=%d", root->type);
            rt_free(json);
            if (cJSON_IsObject(root))
            {
                LOG_I("JSON ready: bytes=%u", (unsigned)bytes);
                return root;
            }
            cJSON_Delete(root);
            root = RT_NULL;
        }
        LOG_W("request failed: HTTP %d, attempt %u", http_status, attempt + 1);
        if (http_status >= 400 && http_status < 500) break;
        if (attempt == 0) rt_thread_mdelay(RETRY_INTERVAL_MS);
    }
    return root;
}

static bool wait_network(void)
{
    if (!btpan_is_connected()) return false;
    btpan_request_connect();
    for (unsigned i = 0; i < 60; ++i)
    {
        if (weather_network_ready()) return true;
        if (!btpan_is_connected()) return false;
        rt_thread_mdelay(250);
    }
    return false;
}

static bool parse_now(cJSON *root, weather_info_t *out)
{
    cJSON *now = cJSON_GetObjectItemCaseSensitive(root, "now");
    if (!api_ok(root) || !cJSON_IsObject(now)) return false;
    out->temperature = number(now, "temp", -100, 100);
    out->code = number(now, "icon", 100, 999);
    if (out->temperature == WEATHER_MISSING || out->code == WEATHER_MISSING ||
        !copy_string(out->text, sizeof(out->text), now, "text")) return false;
    copy_string(out->update_time, sizeof(out->update_time), root, "updateTime");
    out->feels_like = number(now, "feelsLike", -150, 150);
    out->humidity = number(now, "humidity", 0, 100);
    out->wind_speed = number(now, "windSpeed", 0, 1000);
    out->wind_scale = number(now, "windScale", 0, 18);
    out->visibility = number(now, "vis", 0, 1000);
    out->pressure = number(now, "pressure", 0, 2000);
    out->cloud = number(now, "cloud", 0, 100);
    copy_string(out->wind_dir, sizeof(out->wind_dir), now, "windDir");
    copy_string(out->wind_scale_text, sizeof(out->wind_scale_text), now, "windScale");
    return true;
}

static bool parse_daily(cJSON *root, weather_info_t *out)
{
    cJSON *daily = cJSON_GetObjectItemCaseSensitive(root, "daily");
    if (!api_ok(root) || !cJSON_IsArray(daily) || cJSON_GetArraySize(daily) < 4) return false;
    cJSON *today = cJSON_GetArrayItem(daily, 0);
    out->high = number(today, "tempMax", -100, 100);
    out->low = number(today, "tempMin", -100, 100);
    copy_string(out->sunrise, sizeof(out->sunrise), today, "sunrise");
    copy_string(out->sunset, sizeof(out->sunset), today, "sunset");
    if (out->high == WEATHER_MISSING || out->low == WEATHER_MISSING) return false;
    for (unsigned i = 0; i < WEATHER_FORECAST_MAX; ++i)
    {
        cJSON *day = cJSON_GetArrayItem(daily, i + 1);
        weather_forecast_t *forecast = &out->forecast[i];
        forecast->code = number(day, "iconDay", 100, 999);
        forecast->high = number(day, "tempMax", -100, 100);
        forecast->low = number(day, "tempMin", -100, 100);
        if (!copy_string(forecast->date, sizeof(forecast->date), day, "fxDate") ||
            !copy_string(forecast->text, sizeof(forecast->text), day, "textDay") ||
            forecast->code == WEATHER_MISSING || forecast->high == WEATHER_MISSING ||
            forecast->low == WEATHER_MISSING) return false;
        copy_string(forecast->wind_dir, sizeof(forecast->wind_dir), day, "windDirDay");
        copy_string(forecast->wind_scale_text, sizeof(forecast->wind_scale_text), day, "windScaleDay");
        forecast->wind_scale = number(day, "windScaleDay", 0, 18);
        ++out->forecast_count;
    }
    return true;
}

static bool parse_coordinate(cJSON *city, const char *name, int limit, int32_t *out)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(city, name);
    char *end;
    if (!cJSON_IsString(item) || !item->valuestring[0]) return false;
    double value = strtod(item->valuestring, &end);
    if (*end || !isfinite(value) || value < -limit || value > limit) return false;
    *out = (int32_t)(value * 100 + (value >= 0 ? 0.5 : -0.5));
    return true;
}

static bool lookup_city(weather_config_t *cfg, const char *id)
{
    char path[128], returned_id[WEATHER_CITY_ID_MAX];
    LOG_I("city lookup: id=%s", id);
    rt_snprintf(path, sizeof(path), "/geo/v2/city/lookup?location=%s&number=1&lang=zh", id);
    cJSON *root = get_json(cfg, path);
    if (!root) return false;
    cJSON *cities = cJSON_GetObjectItemCaseSensitive(root, "location");
    cJSON *city = cJSON_GetArrayItem(cities, 0);
    bool ok = false;
    if (!api_ok(root)) goto finish;
    if (!cJSON_IsArray(cities) || !cJSON_IsObject(city))
    {
        LOG_W("stage=city-list no location object");
        goto finish;
    }
    if (!copy_string(returned_id, sizeof(returned_id), city, "id") || strcmp(id, returned_id))
    {
        LOG_W("stage=city-id missing or mismatched ID");
        goto finish;
    }
    if (!copy_string(cfg->city, sizeof(cfg->city), city, "name") ||
        !parse_coordinate(city, "lat", 90, &cfg->latitude) ||
        !parse_coordinate(city, "lon", 180, &cfg->longitude))
    {
        LOG_W("stage=city-fields invalid name or coordinates");
        goto finish;
    }
    {
        char province[32] = "", country[32] = "";
        copy_string(province, sizeof(province), city, "adm1");
        copy_string(country, sizeof(country), city, "country");
        rt_snprintf(cfg->city_id, sizeof(cfg->city_id), "%s", id);
        rt_snprintf(cfg->region, sizeof(cfg->region), "%s %s", country, province);
    }
    ok = true;
    LOG_I("city resolved: id=%s name=%s", cfg->city_id, cfg->city);
finish:
    cJSON_Delete(root);
    return ok;
}

static bool run_request(const weather_request_t *request)
{
    weather_config_t candidate;
    weather_info_t snapshot;
    char message[96] = "同步失败，请重试";
    bool ok = false;
    weather_get_config(&candidate);
    weather_get_info(&snapshot);
    LOG_I("job=%u ticket=%u start", (unsigned)request->job, (unsigned)request->ticket);
    if (request->job == WEATHER_JOB_SYNC)
    {
        ok = refresh(&candidate);
        if (ok) rt_snprintf(message, sizeof(message), "同步成功");
    }
    else if (request->job == WEATHER_JOB_IMPORT)
    {
        if (weather_store_import(&candidate, message, sizeof(message)))
        {
            ok = weather_store_save(&candidate, &snapshot);
            if (ok)
            {
                rt_mutex_take(lock, RT_WAITING_FOREVER);
                config = candidate;
                rt_mutex_release(lock);
                rt_snprintf(message, sizeof(message), "配置导入成功");
            }
            else rt_snprintf(message, sizeof(message), "配置保存失败，请重试");
        }
    }
    else if (request->job == WEATHER_JOB_CITY)
    {
        if (!weather_config_valid(&candidate))
        {
            LOG_W("stage=city-config credentials not configured");
            rt_snprintf(message, sizeof(message), "请先导入天气配置");
        }
        else if (!wait_network())
        {
            LOG_W("stage=city-network PAN/IP not ready");
            rt_snprintf(message, sizeof(message), "网络未连接，请重试");
        }
        else if (!lookup_city(&candidate, request->city_id))
            rt_snprintf(message, sizeof(message), "城市查询失败，请重试");
        else
        {
            if (strcmp(candidate.city_id, snapshot.city_id)) empty_info(&snapshot, &candidate);
            rt_snprintf(snapshot.city, sizeof(snapshot.city), "%s", candidate.city);
            rt_snprintf(snapshot.region, sizeof(snapshot.region), "%s", candidate.region);
            ok = weather_store_save(&candidate, &snapshot);
            if (ok)
            {
                publish(&candidate, &snapshot);
                size_t length = strlen(candidate.city);
                const char *suffix = length >= 3 ? candidate.city + length - 3 : "";
                bool has_suffix = !strcmp(suffix, "市") || !strcmp(suffix, "区") || !strcmp(suffix, "县");
                rt_snprintf(message, sizeof(message), "%s%s，设置成功", candidate.city, has_suffix ? "" : "市");
            }
            else
            {
                LOG_W("stage=city-store save failed");
                rt_snprintf(message, sizeof(message), "城市保存失败，请重试");
            }
        }
    }
    rt_mutex_take(lock, RT_WAITING_FOREVER);
    result.busy = false;
    result.success = ok;
    rt_snprintf(result.message, sizeof(result.message), "%s", message);
    rt_mutex_release(lock);
    LOG_I("job=%u ticket=%u success=%d", (unsigned)request->job, (unsigned)request->ticket, ok);
    return ok;
}

static void worker_entry(void *parameter)
{
    rt_tick_t interval = rt_tick_from_millisecond(UPDATE_INTERVAL_MS);
    rt_tick_t retry_interval = rt_tick_from_millisecond(RETRY_INTERVAL_MS);
    rt_tick_t last_period = rt_tick_get();
    rt_tick_t failed_at = 0;
    uint32_t generation = pan_generation_get();
    bool sync_pending = true;
    bool retry_wait = false;
    bool network_was_ready = false;
    (void)parameter;
    for (;;)
    {
        weather_request_t request;
        bool have_request = rt_mq_recv(requests, &request, sizeof(request),
                                       rt_tick_from_millisecond(1000)) == RT_EOK;
        bool network_ready = weather_network_ready();
        uint32_t current_generation = pan_generation_get();
        if (current_generation != generation || (network_ready && !network_was_ready))
        {
            sync_pending = true;
            generation = current_generation;
        }
        network_was_ready = network_ready;

        bool manual_sync = have_request && request.job == WEATHER_JOB_SYNC;
        if (have_request)
        {
            bool ok = run_request(&request);
            if (manual_sync)
            {
                sync_pending = !ok;
                retry_wait = !ok;
                failed_at = rt_tick_get();
                if (ok) network_was_ready = true;
            }
            else if (ok)
            {
                /* Imported credentials or a verified city need fresh weather. */
                sync_pending = true;
                retry_wait = false;
            }
        }

        rt_tick_t now = rt_tick_get();
        bool due = (rt_tick_t)(now - last_period) >= interval;
        if (due)
        {
            last_period += ((rt_tick_t)(now - last_period) / interval) * interval;
            if (!manual_sync) sync_pending = true;
        }
        /* Keep pending work while offline; an expired period is not a completed sync. */
        if (have_request || !sync_pending || !network_ready) continue;
        if (retry_wait && (rt_tick_t)(now - failed_at) < retry_interval) continue;
        weather_config_t current;
        weather_get_config(&current);
        if (!weather_config_valid(&current) || !digits(current.city_id)) continue;

        /* Silent jobs never modify the foreground operation/result ticket. */
        LOG_I("automatic sync start");
        bool ok = refresh(&current);
        sync_pending = !ok;
        retry_wait = !ok;
        failed_at = rt_tick_get();
        if (ok) LOG_I("automatic sync complete");
        else LOG_W("automatic sync pending; retry in %u ms when online", RETRY_INTERVAL_MS);
    }
}

rt_err_t weather_service_init(void)
{
    if (requests) return RT_EOK;
    lock = rt_mutex_create("wx_lock", RT_IPC_FLAG_PRIO);
    if (!lock) return -RT_ENOMEM;
    memset(&config, 0, sizeof(config));
    empty_info(&info, &config);
    if (weather_store_load(&config, &info))
    {
        if (!weather_config_valid(&config) || strcmp(info.city_id, config.city_id) ||
            info.forecast_count < 0 || info.forecast_count > WEATHER_FORECAST_MAX)
            empty_info(&info, &config);
        info.state = info.valid ? WEATHER_STATE_CACHED : WEATHER_STATE_IDLE;
    }
    info.revision = 1;
    requests = rt_mq_create("wx_jobs", sizeof(weather_request_t), 2, RT_IPC_FLAG_FIFO);
    rt_thread_t worker = requests ? rt_thread_create("weather", worker_entry, RT_NULL, 12288, 21, 20) : RT_NULL;
    if (!worker)
    {
        if (requests) rt_mq_delete(requests);
        requests = RT_NULL;
        rt_mutex_delete(lock);
        lock = RT_NULL;
        return -RT_ENOMEM;
    }
    btpan_set_event_cb(pan_state_changed);
    rt_thread_startup(worker);
    LOG_I("weather service ready; interval=30min, retry=5s, cache=%d", info.valid);
    return RT_EOK;
}

rt_err_t weather_request_job(weather_job_t job, const char *city_id, uint32_t *ticket)
{
    weather_request_t request = {0};
    if (!requests) return -RT_ERROR;
    if (job <= WEATHER_JOB_NONE || job > WEATHER_JOB_CITY ||
        (job == WEATHER_JOB_CITY && !digits(city_id))) return -RT_EINVAL;
    rt_mutex_take(lock, RT_WAITING_FOREVER);
    if (result.busy) { rt_mutex_release(lock); return -RT_EBUSY; }
    weather_result_t previous = result;
    memset(&result, 0, sizeof(result));
    if (++next_ticket == 0) ++next_ticket;
    request.job = job;
    request.ticket = next_ticket;
    if (city_id) rt_snprintf(request.city_id, sizeof(request.city_id), "%s", city_id);
    result.job = job;
    result.ticket = request.ticket;
    result.busy = true;
    rt_err_t ret = rt_mq_send(requests, &request, sizeof(request));
    if (ret == RT_EOK) { if (ticket) *ticket = request.ticket; }
    else result = previous;
    rt_mutex_release(lock);
    return ret;
}

void weather_get_result(weather_result_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!lock) return;
    rt_mutex_take(lock, RT_WAITING_FOREVER);
    *out = result;
    rt_mutex_release(lock);
}

void weather_get_config(weather_config_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!lock) return;
    rt_mutex_take(lock, RT_WAITING_FOREVER);
    *out = config;
    rt_mutex_release(lock);
}

bool weather_get_info(weather_info_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!lock) return false;
    rt_mutex_take(lock, RT_WAITING_FOREVER);
    *out = info;
    rt_mutex_release(lock);
    return out->valid;
}

weather_state_t weather_get_state(void)
{
    if (!lock) return WEATHER_STATE_IDLE;
    rt_mutex_take(lock, RT_WAITING_FOREVER);
    weather_state_t state = info.state;
    rt_mutex_release(lock);
    return state;
}

rt_err_t weather_request_refresh(void)
{
    return weather_request_job(WEATHER_JOB_SYNC, RT_NULL, RT_NULL);
}

rt_err_t weather_set_city(const char *city_id)
{
    return weather_request_job(WEATHER_JOB_CITY, city_id, RT_NULL);
}

static void coordinate_text(char *buffer, size_t capacity, int32_t coordinate)
{
    unsigned absolute = coordinate < 0 ? -coordinate : coordinate;
    rt_snprintf(buffer, capacity, "%s%u.%02u", coordinate < 0 ? "-" : "", absolute / 100, absolute % 100);
}

static void fetch_air(const weather_config_t *cfg, weather_info_t *out)
{
    char path[128], latitude[16], longitude[16];
    coordinate_text(latitude, sizeof(latitude), cfg->latitude);
    coordinate_text(longitude, sizeof(longitude), cfg->longitude);
    rt_snprintf(path, sizeof(path), "/airquality/v1/current/%s/%s?lang=zh", latitude, longitude);
    cJSON *root = get_json(cfg, path);
    cJSON *indexes = cJSON_GetObjectItemCaseSensitive(root, "indexes"), *item;
    /* Keep China's AQI scale; do not substitute a different international index. */
    cJSON_ArrayForEach(item, indexes)
    {
        cJSON *code = cJSON_GetObjectItemCaseSensitive(item, "code");
        if (cJSON_IsString(code) && !strcmp(code->valuestring, "cn-mee"))
        {
            int value = number(item, "aqi", 0, 500);
            if (value != WEATHER_MISSING)
            {
                out->aqi = value;
                copy_string(out->aqi_category, sizeof(out->aqi_category), item, "category");
            }
            break;
        }
    }
    cJSON_Delete(root);
}

static void publish(const weather_config_t *cfg, weather_info_t *data)
{
    rt_mutex_take(lock, RT_WAITING_FOREVER);
    data->revision = info.revision + 1;
    config = *cfg;
    info = *data;
    rt_mutex_release(lock);
}

static bool refresh(const weather_config_t *cfg)
{
    weather_info_t fresh;
    char path[128];
    bool ok = false;
    rt_mutex_take(lock, RT_WAITING_FOREVER);
    info.state = WEATHER_STATE_REFRESHING;
    rt_mutex_release(lock);
    if (!weather_config_valid(cfg) || !digits(cfg->city_id) || !wait_network()) goto finish;
    empty_info(&fresh, cfg);
    rt_snprintf(path, sizeof(path), "/v7/weather/now?location=%s&lang=zh", cfg->city_id);
    cJSON *root = get_json(cfg, path);
    bool parsed = parse_now(root, &fresh);
    cJSON_Delete(root);
    if (!parsed) goto finish;
    rt_snprintf(path, sizeof(path), "/v7/weather/7d?location=%s&lang=zh", cfg->city_id);
    root = get_json(cfg, path);
    parsed = parse_daily(root, &fresh);
    cJSON_Delete(root);
    if (!parsed) goto finish;
    fetch_air(cfg, &fresh);
    fresh.valid = true;
    fresh.state = WEATHER_STATE_UPDATED;
    rt_snprintf(fresh.status, sizeof(fresh.status), "同步成功");
    if (!weather_store_save(cfg, &fresh)) goto finish;
    publish(cfg, &fresh);
    ok = true;
finish:
    if (!ok)
    {
        rt_mutex_take(lock, RT_WAITING_FOREVER);
        info.state = info.valid ? WEATHER_STATE_CACHED : WEATHER_STATE_FAILED;
        rt_snprintf(info.status, sizeof(info.status), "同步失败，请重试");
        rt_mutex_release(lock);
        LOG_W("weather sync failed; previous snapshot retained");
    }
    return ok;
}
