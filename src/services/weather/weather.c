/*
 * SPDX-FileCopyrightText: 2024-2025 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/**
 * @file weather.c
 * @brief 天气服务实现
 *
 * 流程：weather_request_refresh()
 *   → worker 线程：确保 PAN 网络（已连则等待；未连则自动请求并等待）
 *   → DNS 可达性探测
 *   → HTTP 拉取「实况 now」+「预报 daily」→ 解析
 *   → 更新快照 → 状态通知（UPDATED / CACHED / FAILED）
 */
#include "weather.h"

#include <string.h>
#include <stdlib.h>

#include "lwip/api.h"
#include "lwip/dns.h"
#include <webclient.h>
#include <cJSON.h>

#include "bt_pan.h"

#define DBG_TAG "weather"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/*---------------------------------------------------------------------------*/
/* 数据源配置（心知天气） */
/*---------------------------------------------------------------------------*/
#define WEATHER_HOST            "api.seniverse.com"
#define WEATHER_KEY             "SO23_Gmly2oK3kMf4"
#define WEATHER_LANGUAGE        "zh-Hans"
#define WEATHER_UNIT            "c"

#define WEATHER_NOW_URL         "http://" WEATHER_HOST "/v3/weather/now.json?key=%s&location=%s&language=" WEATHER_LANGUAGE "&unit=" WEATHER_UNIT
/* 拉 4 天，取第 2~4 天作为“未来三天”预报（forecast[]） */
#define WEATHER_DAILY_URL       "http://" WEATHER_HOST "/v3/weather/daily.json?key=%s&location=%s&language=" WEATHER_LANGUAGE "&unit=" WEATHER_UNIT "&start=0&days=4"
#define WEATHER_URL_LEN_MAX     256
#define CITY_ID_MAX             32

/* 城市名最大长度（城市选择表显示名） */
#define CITY_NAME_MAX           24

/*---------------------------------------------------------------------------*/
/* 预设城市（城市选择页） */
/*---------------------------------------------------------------------------*/
static const weather_city_t g_city_list[] =
{
    { "北京", "beijing" },
    { "上海", "shanghai" },
    { "南京", "nanjing" },
    { "深圳", "shenzhen" },
    { "杭州", "hangzhou" },
    { "成都", "chengdu" },
    { "广州", "guangzhou" },
    { "西安", "xian" },
};

#define CITY_COUNT (sizeof(g_city_list) / sizeof(g_city_list[0]))

/*---------------------------------------------------------------------------*/
/* 服务内部状态 */
/*---------------------------------------------------------------------------*/
typedef enum
{
    WEATHER_MSG_REFRESH = 1,
} weather_msg_t;

static weather_info_t g_info;                /* 数据快照（g_lock 保护） */
static rt_mutex_t g_lock;
static rt_mailbox_t g_mailbox;
static rt_thread_t g_worker;
static weather_event_cb_t g_event_cb;
static volatile rt_bool_t g_refresh_busy;    /* 合并连续刷新请求 */
static char g_city_id[CITY_ID_MAX] = "nanjing";

/*---------------------------------------------------------------------------*/
/* 状态与通知 */
/*---------------------------------------------------------------------------*/
static void weather_set_state(weather_state_t state, const char *status)
{
    if (g_lock != RT_NULL)
        rt_mutex_take(g_lock, RT_WAITING_FOREVER);

    g_info.state = state;
    if (status != RT_NULL)
        rt_snprintf(g_info.status, sizeof(g_info.status), "%s", status);

    if (g_lock != RT_NULL)
        rt_mutex_release(g_lock);

    /* 回调在锁外调用，允许回调里安全调用本模块其它接口 */
    if (g_event_cb != RT_NULL)
        g_event_cb(state);
}

/* 刷新失败：有旧数据 -> CACHED（继续显示缓存）；无数据 -> FAILED */
static void weather_finish_failure(const char *status)
{
    if (g_info.valid)
        weather_set_state(WEATHER_STATE_CACHED, status);
    else
        weather_set_state(WEATHER_STATE_FAILED, status);
}

/*---------------------------------------------------------------------------*/
/* 网络辅助 */
/*---------------------------------------------------------------------------*/
/**
 * @brief 确保 PAN 网络可用（手机已连则等待；未连则自动请求并等待）
 * @param status_out 失败时返回原因文本（直接展示给用户）
 * @return 0 可用；-1 失败
 */
static int weather_wait_network(const char **status_out)
{
    int i;

    if (btpan_is_network_ready())
        return 0;

    if (!btpan_is_connected())
    {
        *status_out = "蓝牙未连接";
        return -1;
    }

    /* 手机链路已建立但 PAN 未连通：请求连接并等待（最多 5s） */
    btpan_request_connect();
    for (i = 0; i < 20; i++)
    {
        if (btpan_is_network_ready())
            return 0;

        if (!btpan_is_connected())
        {
            *status_out = "蓝牙未连接";
            return -1;
        }

        rt_thread_mdelay(250);
    }

    *status_out = "网络未连接";
    return -1;
}

/**
 * @brief DNS 探测：判断本机能否解析天气服务器域名。
 */
static int weather_dns_check(void)
{
    ip_addr_t addr = {0};
    err_t err = dns_gethostbyname(WEATHER_HOST, &addr, RT_NULL, RT_NULL);

    return (err == ERR_OK || err == ERR_INPROGRESS) ? 1 : 0;
}

static int weather_wait_internet(int retry, int delay_ms)
{
    int i;

    for (i = 0; i < retry; i++)
    {
        if (weather_dns_check())
            return 0;
        rt_thread_mdelay(delay_ms);
    }

    return -1;
}

/**
 * @brief HTTP GET，返回的 buffer 需 web_free 释放。
 */
static char *weather_http_get(const char *url)
{
    char *buffer = RT_NULL;
    size_t resp_len = 0;
    int ret = webclient_request((char *)url, RT_NULL, RT_NULL, 0, (void **)&buffer, &resp_len);

    if (ret < 0 || buffer == RT_NULL || resp_len == 0)
    {
        if (buffer != RT_NULL)
            web_free(buffer);
        return RT_NULL;
    }

    return buffer;
}

/*---------------------------------------------------------------------------*/
/* JSON 解析 */
/*---------------------------------------------------------------------------*/
static int json_to_int(cJSON *item, int def)
{
    if (item == RT_NULL)
        return def;

    if (cJSON_IsNumber(item))
        return (int)item->valuedouble;

    if (cJSON_IsString(item) && item->valuestring != RT_NULL)
        return atoi(item->valuestring);

    return def;
}

static void json_to_str(char *dst, rt_size_t dst_size, cJSON *item)
{
    if (dst == RT_NULL || dst_size == 0)
        return;

    if (item != RT_NULL && cJSON_IsString(item) && item->valuestring != RT_NULL)
        rt_snprintf(dst, dst_size, "%s", item->valuestring);
    else
        dst[0] = '\0';
}

/**
 * @brief 从 results[0] 取公共节点
 */
static cJSON *parse_result0(cJSON *root, cJSON **out_location, cJSON **out_now)
{
    cJSON *results = cJSON_GetObjectItem(root, "results");

    if (!cJSON_IsArray(results) || cJSON_GetArraySize(results) <= 0)
        return RT_NULL;

    cJSON *result_item = cJSON_GetArrayItem(results, 0);
    if (result_item == RT_NULL)
        return RT_NULL;

    if (out_location != RT_NULL)
        *out_location = cJSON_GetObjectItem(result_item, "location");
    if (out_now != RT_NULL)
        *out_now = cJSON_GetObjectItem(result_item, "now");

    return result_item;
}

/**
 * @brief 解析实况（now.json）到快照。
 * @return 0 成功
 */
static int weather_parse_now(const char *json, weather_info_t *out)
{
    cJSON *root = cJSON_Parse(json);
    cJSON *location = RT_NULL;
    cJSON *now = RT_NULL;
    cJSON *result_item;
    const char *last_update;

    if (root == RT_NULL)
    {
        LOG_W("now json parse error");
        return -1;
    }

    result_item = parse_result0(root, &location, &now);
    if (result_item == RT_NULL || location == RT_NULL || now == RT_NULL)
    {
        LOG_W("now json fields missing");
        cJSON_Delete(root);
        return -1;
    }

    json_to_str(out->city, sizeof(out->city), cJSON_GetObjectItem(location, "name"));
    json_to_str(out->text, sizeof(out->text), cJSON_GetObjectItem(now, "text"));
    out->code         = json_to_int(cJSON_GetObjectItem(now, "code"), 0);
    out->temperature  = json_to_int(cJSON_GetObjectItem(now, "temperature"), 0);
    out->humidity     = json_to_int(cJSON_GetObjectItem(now, "humidity"), 0);
    json_to_str(out->wind_dir, sizeof(out->wind_dir), cJSON_GetObjectItem(now, "wind_direction"));
    out->wind_speed   = json_to_int(cJSON_GetObjectItem(now, "wind_speed"), 0);
    out->wind_scale   = json_to_int(cJSON_GetObjectItem(now, "wind_scale"), 0);
    out->visibility   = json_to_int(cJSON_GetObjectItem(now, "visibility"), -1);
    out->pressure     = json_to_int(cJSON_GetObjectItem(now, "pressure"), -1);
    out->cloud        = json_to_int(cJSON_GetObjectItem(now, "cloud"), 0);

    /* last_update: "2026-09-22T17:05:00+08:00" -> "17:05" */
    last_update = RT_NULL;
    {
        cJSON *lu = cJSON_GetObjectItem(result_item, "last_update");
        if (lu != RT_NULL && cJSON_IsString(lu) && lu->valuestring != RT_NULL)
            last_update = lu->valuestring;
    }
    if (last_update != RT_NULL)
    {
        const char *t = strchr(last_update, 'T');
        if (t != RT_NULL && strlen(t) >= 6)
            rt_snprintf(out->update_time, sizeof(out->update_time), "%.5s", t + 1);
        else
            rt_snprintf(out->update_time, sizeof(out->update_time), "%.16s", last_update);
    }

    /* 数据源未提供的字段：体感取实测温度 */
    out->feels_like = out->temperature;
    out->aqi = -1;

    cJSON_Delete(root);
    return 0;
}

/**
 * @brief 解析预报（daily.json）到快照。
 * @note daily[0] 为今天 -> 提供今日高低温；daily[1..3] -> forecast[]（未来三天）。
 * @return 0 成功
 */
static int weather_parse_daily(const char *json, weather_info_t *out)
{
    cJSON *root = cJSON_Parse(json);
    cJSON *result_item;
    cJSON *daily;
    int count;
    int i;

    if (root == RT_NULL)
    {
        LOG_W("daily json parse error");
        return -1;
    }

    result_item = parse_result0(root, RT_NULL, RT_NULL);
    if (result_item == RT_NULL)
    {
        cJSON_Delete(root);
        return -1;
    }

    daily = cJSON_GetObjectItem(result_item, "daily");
    if (!cJSON_IsArray(daily))
    {
        cJSON_Delete(root);
        return -1;
    }

    count = cJSON_GetArraySize(daily);

    /* 今天的高低温 */
    if (count >= 1)
    {
        cJSON *today = cJSON_GetArrayItem(daily, 0);
        out->high = json_to_int(cJSON_GetObjectItem(today, "high"), out->high);
        out->low  = json_to_int(cJSON_GetObjectItem(today, "low"), out->low);
    }

    /* 未来三天（跳过今天） */
    out->forecast_count = 0;
    for (i = 1; i < count && out->forecast_count < WEATHER_FORECAST_MAX; i++)
    {
        cJSON *item = cJSON_GetArrayItem(daily, i);
        weather_forecast_t *fc = &out->forecast[out->forecast_count];
        const char *date;

        if (item == RT_NULL)
            continue;

        memset(fc, 0, sizeof(*fc));
        date = RT_NULL;
        {
            cJSON *d = cJSON_GetObjectItem(item, "date");
            if (d != RT_NULL && cJSON_IsString(d) && d->valuestring != RT_NULL)
                date = d->valuestring;
        }
        if (date != RT_NULL)
        {
            /* "2026-09-23" -> "09-23" */
            if (strlen(date) >= 10)
                rt_snprintf(fc->date, sizeof(fc->date), "%.5s", date + 5);
            else
                rt_snprintf(fc->date, sizeof(fc->date), "%s", date);
        }

        json_to_str(fc->text, sizeof(fc->text), cJSON_GetObjectItem(item, "text_day"));
        fc->code       = json_to_int(cJSON_GetObjectItem(item, "code_day"), 0);
        fc->high       = json_to_int(cJSON_GetObjectItem(item, "high"), 0);
        fc->low        = json_to_int(cJSON_GetObjectItem(item, "low"), 0);
        json_to_str(fc->wind_dir, sizeof(fc->wind_dir), cJSON_GetObjectItem(item, "wind_direction"));
        fc->wind_scale = json_to_int(cJSON_GetObjectItem(item, "wind_scale"), 0);

        out->forecast_count++;
    }

    cJSON_Delete(root);
    return 0;
}

/*---------------------------------------------------------------------------*/
/* 刷新流程（worker 线程） */
/*---------------------------------------------------------------------------*/
static void weather_do_refresh(void)
{
    const char *fail_status = RT_NULL;
    char url[WEATHER_URL_LEN_MAX];
    char *json;
    weather_info_t parsed;

    weather_set_state(WEATHER_STATE_REFRESHING, "天气更新中");

    /* 1. 确保 PAN 网络 */
    if (weather_wait_network(&fail_status) != 0)
    {
        weather_finish_failure(fail_status);
        return;
    }

    /* 2. 等网络栈可达（DNS 探测，最多 8 x 500ms） */
    weather_set_state(WEATHER_STATE_REFRESHING, "正在获取数据");
    if (weather_wait_internet(8, 500) != 0)
    {
        weather_finish_failure("网络未就绪");
        return;
    }

    memset(&parsed, 0, sizeof(parsed));

    /* 3. 实况 */
    rt_snprintf(url, sizeof(url), WEATHER_NOW_URL, WEATHER_KEY, g_city_id);
    json = weather_http_get(url);
    if (json == RT_NULL)
    {
        LOG_W("now request failed, retry");
        rt_thread_mdelay(1000);
        json = weather_http_get(url);
    }
    if (json == RT_NULL || weather_parse_now(json, &parsed) != 0)
    {
        if (json != RT_NULL)
            web_free(json);
        weather_finish_failure("天气请求失败");
        return;
    }
    web_free(json);

    /* 4. 预报（失败不影响实况显示） */
    rt_snprintf(url, sizeof(url), WEATHER_DAILY_URL, WEATHER_KEY, g_city_id);
    json = weather_http_get(url);
    if (json != RT_NULL)
    {
        weather_parse_daily(json, &parsed);
        web_free(json);
    }

    /* 5. 提交快照并通知 */
    parsed.valid = true;
    if (g_lock != RT_NULL)
        rt_mutex_take(g_lock, RT_WAITING_FOREVER);
    g_info = parsed;
    if (g_lock != RT_NULL)
        rt_mutex_release(g_lock);

    weather_set_state(WEATHER_STATE_UPDATED, "更新成功");
    LOG_I("weather updated: %s %s %dC", g_info.city, g_info.text, g_info.temperature);
}

static void weather_worker_entry(void *parameter)
{
    rt_uint32_t msg;

    (void)parameter;

    while (1)
    {
        if (rt_mb_recv(g_mailbox, &msg, RT_WAITING_FOREVER) != RT_EOK)
            continue;

        if (msg == WEATHER_MSG_REFRESH)
        {
            weather_do_refresh();
            g_refresh_busy = RT_FALSE;
        }
    }
}

/*---------------------------------------------------------------------------*/
/* 对外接口 */
/*---------------------------------------------------------------------------*/
rt_err_t weather_service_init(void)
{
    if (g_mailbox != RT_NULL)
        return RT_EOK;

    g_lock = rt_mutex_create("wx_lock", RT_IPC_FLAG_PRIO);
    if (g_lock == RT_NULL)
        return -RT_ENOMEM;

    memset(&g_info, 0, sizeof(g_info));
    g_info.state = WEATHER_STATE_IDLE;
    rt_snprintf(g_info.city, sizeof(g_info.city), "%s", g_city_list[2].name);
    rt_snprintf(g_info.status, sizeof(g_info.status), "尚未获取天气");

    g_mailbox = rt_mb_create("wx_mb", 4, RT_IPC_FLAG_FIFO);
    if (g_mailbox == RT_NULL)
        return -RT_ENOMEM;

    g_worker = rt_thread_create("weather",
                                weather_worker_entry,
                                RT_NULL,
                                8192,
                                21,
                                20);
    if (g_worker == RT_NULL)
    {
        rt_mb_delete(g_mailbox);
        g_mailbox = RT_NULL;
        return -RT_ENOMEM;
    }

    rt_thread_startup(g_worker);
    LOG_I("weather service started");
    return RT_EOK;
}

rt_err_t weather_request_refresh(void)
{
    if (g_mailbox == RT_NULL)
        return -RT_ERROR;

    if (g_refresh_busy)
        return RT_EOK; /* 已有刷新在途，合并请求 */

    g_refresh_busy = RT_TRUE;
    if (rt_mb_send(g_mailbox, WEATHER_MSG_REFRESH) != RT_EOK)
    {
        g_refresh_busy = RT_FALSE;
        return -RT_EFULL;
    }

    return RT_EOK;
}

bool weather_get_info(weather_info_t *out)
{
    if (out == RT_NULL || g_lock == RT_NULL)
        return false;

    rt_mutex_take(g_lock, RT_WAITING_FOREVER);
    *out = g_info;
    rt_mutex_release(g_lock);

    return out->valid;
}

weather_state_t weather_get_state(void)
{
    weather_state_t state;

    if (g_lock == RT_NULL)
        return WEATHER_STATE_IDLE;

    rt_mutex_take(g_lock, RT_WAITING_FOREVER);
    state = g_info.state;
    rt_mutex_release(g_lock);

    return state;
}

void weather_set_event_cb(weather_event_cb_t cb)
{
    g_event_cb = cb;
}

const weather_city_t *weather_get_city_list(int *count)
{
    if (count != RT_NULL)
        *count = (int)CITY_COUNT;

    return g_city_list;
}

rt_err_t weather_set_city(const char *city_id)
{
    int i;

    if (city_id == RT_NULL || city_id[0] == '\0')
        return -RT_EINVAL;

    for (i = 0; i < (int)CITY_COUNT; i++)
    {
        if (strcmp(city_id, g_city_list[i].id) == 0)
        {
            rt_snprintf(g_city_id, sizeof(g_city_id), "%s", city_id);

            /* 立即更新显示名（UI 城市选择后无需等刷新） */
            if (g_lock != RT_NULL)
                rt_mutex_take(g_lock, RT_WAITING_FOREVER);
            rt_snprintf(g_info.city, sizeof(g_info.city), "%s", g_city_list[i].name);
            if (g_lock != RT_NULL)
                rt_mutex_release(g_lock);

            return RT_EOK;
        }
    }

    return -RT_EINVAL;
}

const char *weather_get_city(void)
{
    return g_city_id;
}
