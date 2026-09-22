/**
 * @file bt_pan.h
 * @brief 蓝牙 PAN 联网服务 —— 对外接口（UI / 天气服务调用）
 *
 * 功能：设备开机后蓝牙可被手机发现、配对、连接；
 *       手机连接并共享网络后，设备作为 PAN 客户端（PANU）接入，
 *       为整机提供互联网出口（天气等数据请求走此链路）。
 *
 * 典型状态流转：
 *   OFF → INITIALIZING → READY（等待手机连接）
 *       → CONNECTED（手机已连接，链路建立）
 *       → NETWORK_READY（PAN 网络已通，可以访问互联网）
 *
 * 线程说明：
 *  - 所有接口线程安全，可在任意线程 / UI 线程调用；
 *  - 事件回调运行在蓝牙服务线程上下文，UI 中请勿直接操作 LVGL，
 *    应通过 lv_async_call() 或消息队列切回 UI 线程后再刷新界面。
 *
 * 命名说明：对外前缀 btpan_（SDK 中 bt_pan_ 前缀已被占用，避免符号冲突）。
 */
#ifndef __BT_PAN_H__
#define __BT_PAN_H__

#include <rtthread.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 蓝牙 PAN 服务状态 */
typedef enum
{
    BTPAN_STATE_OFF = 0,       /**< 未初始化 / 设置页已关闭蓝牙 */
    BTPAN_STATE_INITIALIZING,  /**< 蓝牙协议栈启动中 */
    BTPAN_STATE_READY,         /**< 蓝牙就绪，等待手机连接 */
    BTPAN_STATE_CONNECTED,     /**< 手机已连接（链路建立），网络未就绪 */
    BTPAN_STATE_NETWORK_READY, /**< PAN 网络已连通，可访问互联网 */
} btpan_state_t;

/** 状态变化回调（服务线程上下文调用，UI 请转投 UI 线程） */
typedef void (*btpan_event_cb_t)(btpan_state_t state);

/* ==================== 生命周期 ==================== */

/**
 * @brief 初始化 PAN 服务（应用入口调用一次）
 * @param device_name 蓝牙设备名（手机端显示的名称），NULL 使用默认值
 * @return RT_EOK 成功
 */
rt_err_t btpan_init(const char *device_name);

/**
 * @brief 蓝牙开关（对应设置页“蓝牙”开关；关闭会断开当前连接）
 * @param enable true 打开（恢复可被发现/连接），false 关闭
 */
rt_err_t btpan_enable(bool enable);

/* ==================== 连接控制 ==================== */

/**
 * @brief 主动请求建立 PAN 连接（手机已配对连接后调用）
 * @note 手机刚配对（或有历史配对）且链路已建立时，由本接口发起 PAN 连接；
 *       天气服务在需要网络时会自行调用，UI 通常无需直接调用。
 */
rt_err_t btpan_request_connect(void);

/* ==================== 状态查询（供 UI 状态栏 / 设置页） ==================== */

/** 当前状态 */
btpan_state_t btpan_get_state(void);
/** 蓝牙协议栈是否就绪 */
bool btpan_is_ready(void);
/** 手机是否已连接（链路建立） */
bool btpan_is_connected(void);
/** PAN 网络是否已连通（可访问互联网） */
bool btpan_is_network_ready(void);

/* ==================== 事件 ==================== */

/**
 * @brief 注册状态变化回调（可传 NULL 注销）
 * @note 单回调槽位；UI 多方需要时请在 UI 层统一分发。
 */
void btpan_set_event_cb(btpan_event_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* __BT_PAN_H__ */
