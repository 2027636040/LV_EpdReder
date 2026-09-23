/**
 * @file weather.h
 * @brief 天气数据服务 —— 对外接口（UI 调用）
 *
 * 数据源：和风天气 QWeather（WebAPI v7，HTTPS）
 * 网络链路：蓝牙 PAN（手机热点共享）→ HTTPS → 解析 → 快照
 *
 * 用法（UI 侧）：
 *   1. 应用入口调用 weather_service_init() 一次；
 *   2. 进入天气页调用 weather_request_refresh()（异步，立即返回，
 *      内部会自动确保 PAN 网络：连接手机 → 等待网络 → 拉取数据）；
 *   3. 收到事件回调（或轮询状态）后调用 weather_get_info(&info)
 *      拷贝数据并刷新页面；
 *   4. 城市选择页调用 weather_set_city("beijing") 后请求刷新。
 *
 * 线程说明：
 *  - 所有接口线程安全，可在 UI 线程直接调用；
 *  - 事件回调运行在天气服务线程上下文，UI 中请勿直接操作 LVGL，
 *    应通过 lv_async_call() 或消息队列切回 UI 线程后再刷新界面。
 */
#ifndef __WEATHER_H__
#define __WEATHER_H__

#include <rtthread.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 天气服务状态（UI 可直接显示 status 文本） */
typedef enum
{
    WEATHER_STATE_IDLE = 0,   /**< 尚未请求过，无数据 */
    WEATHER_STATE_REFRESHING, /**< 刷新中 */
    WEATHER_STATE_UPDATED,    /**< 已更新（数据来自本次网络请求） */
    WEATHER_STATE_CACHED,     /**< 网络失败，当前显示上次缓存数据 */
    WEATHER_STATE_FAILED,     /**< 失败且无可用数据 */
} weather_state_t;

/** 单日预报 */
typedef struct
{
    char date[16];     /**< 日期文本，如 "09-23"（UI 可再转为“明天/后天”） */
    char text[24];     /**< 天气描述：多云 */
    int  code;         /**< 图标代码（和风，UI 映射图标资源） */
    int  high;         /**< 最高温 ℃ */
    int  low;          /**< 最低温 ℃ */
    char wind_dir[16]; /**< 风向：东南 */
    int  wind_scale;   /**< 风力等级：3 */
} weather_forecast_t;

#define WEATHER_FORECAST_MAX 3

/** 天气数据快照（UI 显示数据全部来自此结构） */
typedef struct
{
    bool valid;            /**< 数据是否可用（false 时其余字段无意义） */
    weather_state_t state; /**< 当前状态 */
    char status[48];       /**< 状态文本（可直接显示：“更新中 / 网络未连接 …”） */

    char city[32];        /**< 城市显示名：南京 */
    char update_time[24]; /**< 数据更新时间："17:05"（数据源提供时） */

    /* ---- 当前天气（主区域） ---- */
    char text[24];  /**< 天气描述：多云 */
    int  code;      /**< 和风天气图标代码（100=晴/305=小雨…，UI 映射图标） */
    int  temperature; /**< 当前温度 ℃ */
    int  feels_like;  /**< 体感温度 ℃（数据源未提供时等于 temperature） */
    int  high;        /**< 今日最高温 ℃ */
    int  low;         /**< 今日最低温 ℃ */

    /* ---- 实况详情（信息卡片） ---- */
    int  humidity;        /**< 湿度 % */
    char wind_dir[16];    /**< 风向：东南 */
    int  wind_scale;      /**< 风力等级：3 */
    int  wind_speed;      /**< 风速 km/h */
    int  visibility;      /**< 能见度 km */
    int  pressure;        /**< 气压 hPa */
    int  cloud;           /**< 云量 %（数据源未提供时 0） */
    int  aqi;             /**< 空气质量指数（数据源未提供时 -1） */
    char aqi_category[8]; /**< 空气质量等级：优/良/…（未提供时为空） */
    char sunrise[8];      /**< 日出 "06:12"（未提供时为空） */
    char sunset[8];       /**< 日落 "18:35"（未提供时为空） */

    /* ---- 未来预报 ---- */
    weather_forecast_t forecast[WEATHER_FORECAST_MAX];
    int forecast_count; /**< 有效预报条数（0 ~ 3） */
} weather_info_t;

/* ==================== 生命周期 ==================== */

/**
 * @brief 初始化天气服务（应用入口调用一次；内部创建 worker 线程）
 */
rt_err_t weather_service_init(void);

/* ==================== 数据请求 ==================== */

/**
 * @brief 请求刷新天气（异步，立即返回）
 * @note 内部自动确保 PAN 网络：手机已连则等待网络就绪后拉取；
 *       手机未连接会在数秒内失败并给出“蓝牙未连接”状态。
 *       结果通过事件回调 / weather_get_state() 通知。
 * @return RT_EOK 请求已受理；负值表示服务未初始化或队列满
 */
rt_err_t weather_request_refresh(void);

/**
 * @brief 拷贝当前天气快照（线程安全）
 * @param out 输出结构
 * @return true 表示有可用数据（out->valid 亦为 true）
 */
bool weather_get_info(weather_info_t *out);

/** 当前状态 */
weather_state_t weather_get_state(void);

/* ==================== 事件 ==================== */

/** 状态变化回调（服务线程上下文；UI 请转投 UI 线程） */
typedef void (*weather_event_cb_t)(weather_state_t state);

/** 注册状态变化回调（可传 NULL 注销） */
void weather_set_event_cb(weather_event_cb_t cb);

/* ==================== 城市 ==================== */

/** 预设城市项（城市选择页使用） */
typedef struct
{
    const char *name; /**< 显示名：南京 */
    const char *id;   /**< 和风 LocationID：101190101 */
} weather_city_t;

/**
 * @brief 获取预设城市表
 * @param count 输出城市数量（可为 NULL）
 * @return 城市数组首地址（常量，勿修改）
 */
const weather_city_t *weather_get_city_list(int *count);

/**
 * @brief 设置当前城市（下次刷新生效；城市选择页“确认”时调用）
 * @param city_id 和风 LocationID（如 "101010100"；`svc weather city` 可列出）
 * @return RT_EOK 成功；-RT_EINVAL 城市不支持
 */
rt_err_t weather_set_city(const char *city_id);

/** 获取当前城市 ID */
const char *weather_get_city(void);

#ifdef __cplusplus
}
#endif

#endif /* __WEATHER_H__ */
