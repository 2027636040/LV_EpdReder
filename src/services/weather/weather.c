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
 *   → HTTPS 拉取「实况 now」+「预报 7d」→ 解析
 *   → 更新快照 → 状态通知（UPDATED / CACHED / FAILED）
 */
#include "weather.h"

#include <string.h>
#include <stdlib.h>

#include "lwip/api.h"
#include "lwip/dns.h"
#include <webclient.h>
#include <cJSON.h>

#include "miniz.h"     /* tinfl：解压和风 gzip 响应 */

#include "bt_pan.h"

#define DBG_TAG "weather"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/*---------------------------------------------------------------------------*/
/* 数据源配置（和风天气 QWeather · WebAPI v7）
 *
 * 部署前替换下面两项（控制台 https://console.qweather.com）：
 *  - WEATHER_API_HOST：控制台-设置 中查看，形如 "abcxyz.qweatherapi.com"（不含 https://）
 *  - WEATHER_KEY     ：控制台-项目-凭据 中添加（API KEY 类型）
 *
 * 说明：仅支持 HTTPS（webclient 内置 mbedtls 客户端，开启证书链校验）；
 *       和风服务端强制 gzip（无视 Accept-Encoding），响应经 weather_gunzip() 解压；
 *       根证书仅携带 Let's Encrypt「ISRG Root X1」：
 *       src/services/weather/certs/（由 proj.conf 的 MbedTLS EXTRA_CERT_DIRS 引入），
 *       换用其它域名时往该目录补 PEM 即可；
 *       内存：ptab.json 已把空闲的 ACPU RAM 并入 HCPU 堆（RT 堆 ≈138KB→≈201KB；
 *       本工程未启用 ACPU，将来若启用 ACPU 需撤回该改动）；
 *       按量计费每月前 5 万次请求免费；
 *       7d 预报依赖账号额度，若返回 401/403 权限错误把 WEATHER_DAILY_DAYS 改为 "3d"。
 */
#define WEATHER_API_HOST        "ka54e77w7k.re.qweatherapi.com"
#define WEATHER_KEY             "0be24ee7fa2a40a297d4e46b0af89f86"

#define WEATHER_NOW_URL         "https://" WEATHER_API_HOST "/v7/weather/now?location=%s&key=%s&lang=zh"
#define WEATHER_DAILY_DAYS      "7d"
#define WEATHER_DAILY_URL       "https://" WEATHER_API_HOST "/v7/weather/" WEATHER_DAILY_DAYS "?location=%s&key=%s&lang=zh"
#define WEATHER_URL_LEN_MAX     384
#define CITY_ID_MAX             32

/* 城市名最大长度（城市选择表显示名） */
#define CITY_NAME_MAX           24

/*---------------------------------------------------------------------------*/
/* 预设城市（城市选择页；id 为和风天气 LocationID） */
/*---------------------------------------------------------------------------*/
static const weather_city_t g_city_list[] =
{
    { "北京", "101010100" },
    { "上海", "101020100" },
    { "南京", "101190101" },
    { "深圳", "101280601" },
    { "杭州", "101210101" },
    { "成都", "101270101" },
    { "广州", "101280101" },
    { "西安", "101110101" },
};

#define CITY_COUNT (sizeof(g_city_list) / sizeof(g_city_list[0]))

/* 城市显示名（按 LocationID 查预设表；查不到时直接显示 id） */
static const char *city_display_name(const char *id)
{
    int i;

    for (i = 0; i < (int)CITY_COUNT; i++)
    {
        if (strcmp(g_city_list[i].id, id) == 0)
            return g_city_list[i].name;
    }
    return id;
}

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
static char g_city_id[CITY_ID_MAX] = "101190101"; /* 默认：南京 */

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
    err_t err = dns_gethostbyname(WEATHER_API_HOST, &addr, RT_NULL, RT_NULL);

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

/* gzip 解压输出上限（防异常数据撑爆内存；和风 now/7d 明文仅 0.5~4KB） */
#define WEATHER_GUNZIP_MAX      (32 * 1024)

/**
 * @brief 解压 gzip 数据（跳过 gzip 头及可选字段，tinfl 解原始 deflate）。
 *
 * 注意：不要直接调用 miniz 的 tinfl_decompress_mem_to_heap()——它在栈上分配
 * ~11KB 的 tinfl_decompressor，而 weather 线程栈仅 8KB，会立即触发硬件栈
 * 溢出（实测 PSPLIM 硬故障，PC 停在 `sub sp, #11008`）。此处改为把
 * decompressor 放堆上，循环逻辑与 miniz 原实现一致。
 *
 * @return '\0' 结尾的数据缓冲（需 web_free 释放）；失败返回 RT_NULL
 */
static char *weather_gunzip(const unsigned char *src, size_t src_len)
{
    size_t off = 10;
    size_t src_ofs = 0;
    size_t out_len = 0;
    size_t out_cap = 0;
    unsigned char flg;
    tinfl_decompressor *d;
    unsigned char *out_buf = RT_NULL;
    rt_bool_t done = RT_FALSE;
    char *out;

    if (src_len < 18 || src[0] != 0x1f || src[1] != 0x8b || src[2] != 8)
        return RT_NULL;

    flg = src[3];
    if (flg & 0x04) /* FEXTRA */
        off = 12 + (size_t)src[10] + ((size_t)src[11] << 8);
    if (flg & 0x08) /* FNAME */
    {
        while (off < src_len && src[off] != 0)
            off++;
        off++;
    }
    if (flg & 0x10) /* FCOMMENT */
    {
        while (off < src_len && src[off] != 0)
            off++;
        off++;
    }
    if (flg & 0x02) /* FHCRC */
        off += 2;

    if (off + 4 >= src_len)
        return RT_NULL;

    d = tinfl_decompressor_alloc();
    if (d == RT_NULL)
        return RT_NULL;

    tinfl_init(d);
    for (;;)
    {
        size_t in_left = (src_len - off) - src_ofs;
        size_t out_left = out_cap - out_len;
        tinfl_status st = tinfl_decompress(d,
                                           src + off + src_ofs, &in_left,
                                           out_buf, out_buf ? (out_buf + out_len) : RT_NULL,
                                           &out_left,
                                           TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);

        if (st < 0 || st == TINFL_STATUS_NEEDS_MORE_INPUT)
            break; /* 数据损坏或不完整 */

        src_ofs += in_left;
        out_len += out_left;

        if (st == TINFL_STATUS_DONE)
        {
            done = RT_TRUE;
            break;
        }

        if (out_len > WEATHER_GUNZIP_MAX)
            break; /* 超过输出上限 */

        {
            size_t new_cap = out_cap ? (out_cap * 2) : 256;
            unsigned char *pnew;

            if (new_cap > WEATHER_GUNZIP_MAX + 64)
                new_cap = WEATHER_GUNZIP_MAX + 64;
            pnew = (unsigned char *)MZ_REALLOC(out_buf, new_cap);
            if (pnew == RT_NULL)
                break;
            out_buf = pnew;
            out_cap = new_cap;
        }
    }
    tinfl_decompressor_free(d);

    if (!done || out_buf == RT_NULL || out_len == 0 || out_len > WEATHER_GUNZIP_MAX)
    {
        if (out_buf != RT_NULL)
            MZ_FREE(out_buf);
        return RT_NULL;
    }

    /* cJSON 需要 '\0' 结尾 */
    out = rt_malloc(out_len + 1);
    if (out != RT_NULL)
    {
        rt_memcpy(out, out_buf, out_len);
        out[out_len] = '\0';
    }
    MZ_FREE(out_buf);

    return out;
}

/**
 * @brief HTTP GET，自动解压 gzip；返回的 buffer 需 web_free 释放。
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

    /* 和风响应强制 gzip：解压后交给 cJSON */
    if (resp_len >= 2 && (unsigned char)buffer[0] == 0x1f && (unsigned char)buffer[1] == 0x8b)
    {
        char *plain = weather_gunzip((const unsigned char *)buffer, resp_len);

        web_free(buffer);
        if (plain == RT_NULL)
        {
            LOG_W("gzip inflate failed (%d bytes)", (int)resp_len);
            return RT_NULL;
        }
        return plain;
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
 * @brief 检查和风天气返回状态码（顶层 "code" == "200" 为成功）
 */
static rt_bool_t qweather_code_ok(cJSON *root)
{
    cJSON *code = cJSON_GetObjectItem(root, "code");

    if (code != RT_NULL && cJSON_IsString(code) && code->valuestring != RT_NULL)
    {
        if (strcmp(code->valuestring, "200") == 0)
            return RT_TRUE;

        LOG_W("qweather error code %s", code->valuestring);
        return RT_FALSE;
    }

    return RT_FALSE;
}

/**
 * @brief 解析实况（now.json）到快照。
 * @return 0 成功
 */
static int weather_parse_now(const char *json, weather_info_t *out)
{
    cJSON *root = cJSON_Parse(json);
    cJSON *now;
    const char *update_time;

    if (root == RT_NULL)
    {
        LOG_W("now json parse error");
        return -1;
    }

    if (!qweather_code_ok(root))
    {
        cJSON_Delete(root);
        return -1;
    }

    now = cJSON_GetObjectItem(root, "now");
    if (!cJSON_IsObject(now))
    {
        LOG_W("now json fields missing");
        cJSON_Delete(root);
        return -1;
    }

    /* 和风接口不返回城市名，用本地城市表回填显示名 */
    rt_snprintf(out->city, sizeof(out->city), "%s", city_display_name(g_city_id));

    json_to_str(out->text, sizeof(out->text), cJSON_GetObjectItem(now, "text"));
    out->code         = json_to_int(cJSON_GetObjectItem(now, "icon"), 0);
    out->temperature  = json_to_int(cJSON_GetObjectItem(now, "temp"), 0);
    out->feels_like   = json_to_int(cJSON_GetObjectItem(now, "feelsLike"), out->temperature);
    out->humidity     = json_to_int(cJSON_GetObjectItem(now, "humidity"), 0);
    json_to_str(out->wind_dir, sizeof(out->wind_dir), cJSON_GetObjectItem(now, "windDir"));
    out->wind_speed   = json_to_int(cJSON_GetObjectItem(now, "windSpeed"), 0);
    out->wind_scale   = json_to_int(cJSON_GetObjectItem(now, "windScale"), 0);
    out->visibility   = json_to_int(cJSON_GetObjectItem(now, "vis"), -1);
    out->pressure     = json_to_int(cJSON_GetObjectItem(now, "pressure"), -1);
    out->cloud        = json_to_int(cJSON_GetObjectItem(now, "cloud"), 0);
    out->aqi          = -1; /* 空气质量需单独的 air 接口（后续扩展） */

    /* updateTime: "2021-11-15T16:35+08:00" -> "16:35" */
    update_time = RT_NULL;
    {
        cJSON *ut = cJSON_GetObjectItem(root, "updateTime");
        if (ut != RT_NULL && cJSON_IsString(ut) && ut->valuestring != RT_NULL)
            update_time = ut->valuestring;
    }
    if (update_time != RT_NULL)
    {
        const char *t = strchr(update_time, 'T');
        if (t != RT_NULL && strlen(t) >= 6)
            rt_snprintf(out->update_time, sizeof(out->update_time), "%.5s", t + 1);
        else
            rt_snprintf(out->update_time, sizeof(out->update_time), "%.16s", update_time);
    }

    cJSON_Delete(root);
    return 0;
}

/**
 * @brief 解析预报（/v7/weather/7d）到快照。
 * @note daily[0] 为今天 -> 今日高低温 + 日出日落；
 *       daily[1..] -> forecast[]（未来三天，最多取 3）。
 * @return 0 成功
 */
static int weather_parse_daily(const char *json, weather_info_t *out)
{
    cJSON *root = cJSON_Parse(json);
    cJSON *daily;
    int count;
    int i;

    if (root == RT_NULL)
    {
        LOG_W("daily json parse error");
        return -1;
    }

    if (!qweather_code_ok(root))
    {
        cJSON_Delete(root);
        return -1;
    }

    daily = cJSON_GetObjectItem(root, "daily");
    if (!cJSON_IsArray(daily))
    {
        cJSON_Delete(root);
        return -1;
    }

    count = cJSON_GetArraySize(daily);

    /* 今天：高低温 + 日出日落 */
    if (count >= 1)
    {
        cJSON *today = cJSON_GetArrayItem(daily, 0);
        out->high = json_to_int(cJSON_GetObjectItem(today, "tempMax"), out->high);
        out->low  = json_to_int(cJSON_GetObjectItem(today, "tempMin"), out->low);
        json_to_str(out->sunrise, sizeof(out->sunrise), cJSON_GetObjectItem(today, "sunrise"));
        json_to_str(out->sunset, sizeof(out->sunset), cJSON_GetObjectItem(today, "sunset"));
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
            cJSON *d = cJSON_GetObjectItem(item, "fxDate");
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

        json_to_str(fc->text, sizeof(fc->text), cJSON_GetObjectItem(item, "textDay"));
        fc->code       = json_to_int(cJSON_GetObjectItem(item, "iconDay"), 0);
        fc->high       = json_to_int(cJSON_GetObjectItem(item, "tempMax"), 0);
        fc->low        = json_to_int(cJSON_GetObjectItem(item, "tempMin"), 0);
        json_to_str(fc->wind_dir, sizeof(fc->wind_dir), cJSON_GetObjectItem(item, "windDirDay"));
        /* 风力为区间值（如 "1-2"），取整数部分 */
        fc->wind_scale = json_to_int(cJSON_GetObjectItem(item, "windScaleDay"), 0);

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
    rt_snprintf(url, sizeof(url), WEATHER_NOW_URL, g_city_id, WEATHER_KEY);
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
    rt_snprintf(url, sizeof(url), WEATHER_DAILY_URL, g_city_id, WEATHER_KEY);
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
