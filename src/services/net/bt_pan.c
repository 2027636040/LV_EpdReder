/*
 * SPDX-FileCopyrightText: 2024-2025 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/**
 * @file bt_pan.c
 * @brief 蓝牙 PAN 联网服务实现
 *
 * 链路：手机配对连接（ACL/加密）→ 延迟发起 PAN 连接（手机作 NAP）
 *       → PAN 连通后设备获得网络出口。
 *
 * 稳定性：配对过的设备断线后自动恢复可连接状态并周期重连（10s × 6 次）；
 *         重连定时器同时会复位挂起的连接请求，避免事件丢失后卡死。
 *
 * 注意：对外符号统一使用 btpan_ 前缀（SDK 已占用 bt_pan_ 前缀）。
 */
#include "bt_pan.h"

#include <string.h>

#include "bts2_app_inc.h"
#include "ble_connection_manager.h"
#include "bt_connection_manager.h"
#include "ulog.h"
#include "bf0_ble_common.h" /* bd_addr_t / ble_common_update_type_t / ble_get_public_address */

/*---------------------------------------------------------------------------*/
/* 配置常量 */
/*---------------------------------------------------------------------------*/
#define PAN_TIMER_MS           3000
#define PAN_THREAD_STACK_SIZE  4096
#define PAN_THREAD_PRIORITY    20
#define PAN_THREAD_TICK        20
#define PAN_LOCAL_NAME_MAX     32

/* 断开后的自动重连：每 10s 尝试一次，最多 6 次 */
#define PAN_RECONNECT_DELAY_MS 10000
#define PAN_RECONNECT_MAX      6

/*---------------------------------------------------------------------------*/
/* 内部消息 */
/*---------------------------------------------------------------------------*/
typedef enum
{
    PAN_MSG_STACK_READY = 1, /* 蓝牙协议栈初始化完成 */
    PAN_MSG_CONNECT_PAN = 2, /* 发起 PAN 连接（由定时器/外部请求触发） */
} pan_msg_t;

/*---------------------------------------------------------------------------*/
/* 模块状态 */
/*---------------------------------------------------------------------------*/
typedef struct
{
    rt_bool_t initialized;
    rt_bool_t enabled;      /* 设置页蓝牙开关 */
    rt_bool_t stack_ready;
    rt_bool_t bt_connected; /* 手机链路已建立 */
    rt_bool_t pan_connected;
    rt_bool_t connect_pending;
    bt_notify_device_mac_t bd_addr;
    rt_mailbox_t mailbox;
    rt_timer_t pan_connect_timer;
    rt_timer_t reconnect_timer;
    rt_uint8_t reconnect_attempts;
    rt_thread_t worker;
    char local_name[PAN_LOCAL_NAME_MAX];
    btpan_event_cb_t event_cb;
    btpan_state_t last_state;
} btpan_service_t;

static btpan_service_t g_pan;

/*---------------------------------------------------------------------------*/
/* 状态计算与通知 */
/*---------------------------------------------------------------------------*/
static btpan_state_t btpan_current_state(void)
{
    if (!g_pan.initialized || !g_pan.enabled)
        return BTPAN_STATE_OFF;

    if (!g_pan.stack_ready)
        return BTPAN_STATE_INITIALIZING;

    if (g_pan.pan_connected)
        return BTPAN_STATE_NETWORK_READY;

    if (g_pan.bt_connected)
        return BTPAN_STATE_CONNECTED;

    return BTPAN_STATE_READY;
}

static void btpan_notify_state(void)
{
    btpan_state_t state = btpan_current_state();

    if (state == g_pan.last_state)
        return;

    g_pan.last_state = state;
    if (g_pan.event_cb != RT_NULL)
        g_pan.event_cb(state);
}

/*---------------------------------------------------------------------------*/
/* 内部工具函数 */
/*---------------------------------------------------------------------------*/
static rt_bool_t btpan_has_peer_addr(void)
{
    uint8_t index;

    for (index = 0; index < sizeof(g_pan.bd_addr.addr); index++)
    {
        if (g_pan.bd_addr.addr[index] != 0x00 && g_pan.bd_addr.addr[index] != 0xFF)
            return RT_TRUE;
    }

    return RT_FALSE;
}

static void btpan_reset_mailbox(void)
{
    if (g_pan.mailbox != RT_NULL)
        rt_mb_control(g_pan.mailbox, RT_IPC_CMD_RESET, RT_NULL);

    g_pan.connect_pending = RT_FALSE;
}

/**
 * @brief 向 PAN 工作线程投递内部事件。
 */
static rt_err_t btpan_post(pan_msg_t event)
{
    if (g_pan.mailbox == RT_NULL)
        return -RT_ERROR;

    return rt_mb_send(g_pan.mailbox, (rt_ubase_t)event);
}

static void btpan_stop_timer(void)
{
    if (g_pan.pan_connect_timer != RT_NULL)
        rt_timer_stop(g_pan.pan_connect_timer);
}

/**
 * @brief PAN 连接延迟定时器回调：配对/加密完成后稍后发起 PAN 连接。
 */
static void btpan_connect_timeout(void *parameter)
{
    (void)parameter;

    if (g_pan.bt_connected)
        btpan_request_connect();
}

static void btpan_start_timer(void)
{
    if (g_pan.pan_connect_timer == RT_NULL)
    {
        g_pan.pan_connect_timer = rt_timer_create("connect_pan",
                                                  btpan_connect_timeout,
                                                  RT_NULL,
                                                  rt_tick_from_millisecond(PAN_TIMER_MS),
                                                  RT_TIMER_FLAG_SOFT_TIMER);
    }
    else
    {
        rt_timer_stop(g_pan.pan_connect_timer);
    }

    if (g_pan.pan_connect_timer != RT_NULL)
        rt_timer_start(g_pan.pan_connect_timer);
}

/*---------------------------------------------------------------------------*/
/* 断开自动重连 */
/*---------------------------------------------------------------------------*/
/**
 * @brief 重连定时器回调：对已配对设备周期性重发 PAN 连接请求。
 * @note 运行在软件定时器线程；连接恢复后在事件回调中停止本定时器。
 */
static void btpan_reconnect_timeout(void *parameter)
{
    (void)parameter;

    /* 事件丢失保护：复位挂起标志，允许重新发起 */
    g_pan.connect_pending = RT_FALSE;

    if (!g_pan.enabled || !btpan_has_peer_addr())
        return;

    if (g_pan.bt_connected || g_pan.pan_connected)
        return; /* 已恢复 */

    if (g_pan.reconnect_attempts >= PAN_RECONNECT_MAX)
    {
        LOG_I("pan reconnect give up (%d attempts)", g_pan.reconnect_attempts);
        rt_timer_stop(g_pan.reconnect_timer);
        return;
    }

    g_pan.reconnect_attempts++;
    LOG_I("pan reconnect attempt %d/%d", g_pan.reconnect_attempts, PAN_RECONNECT_MAX);

    /* 重新建立链路并连接 PAN（对已配对地址） */
    bt_interface_conn_ext((char *)&g_pan.bd_addr, BT_PROFILE_PAN);
}

static void btpan_stop_reconnect(void)
{
    if (g_pan.reconnect_timer != RT_NULL)
        rt_timer_stop(g_pan.reconnect_timer);

    g_pan.reconnect_attempts = 0;
}

static void btpan_start_reconnect(void)
{
    g_pan.reconnect_attempts = 0;

    if (g_pan.reconnect_timer == RT_NULL)
    {
        g_pan.reconnect_timer = rt_timer_create("pan_recon",
                                                btpan_reconnect_timeout,
                                                RT_NULL,
                                                rt_tick_from_millisecond(PAN_RECONNECT_DELAY_MS),
                                                RT_TIMER_FLAG_PERIODIC | RT_TIMER_FLAG_SOFT_TIMER);
    }

    if (g_pan.reconnect_timer != RT_NULL)
        rt_timer_start(g_pan.reconnect_timer);
}

/*---------------------------------------------------------------------------*/
/* 蓝牙事件回调 */
/*---------------------------------------------------------------------------*/
/**
 * @brief 处理蓝牙协议栈上报的公共事件与 PAN profile 事件。
 */
static int btpan_bt_event_handle(uint16_t type, uint16_t event_id, uint8_t *data, uint16_t data_len)
{
    (void)data_len;

    /* ---- 蓝牙公共事件 ---- */
    if (type == BT_NOTIFY_COMMON)
    {
        rt_bool_t should_connect_pan = RT_FALSE;

        switch (event_id)
        {
        case BT_NOTIFY_COMMON_BT_STACK_READY:
            btpan_post(PAN_MSG_STACK_READY);
            break;

        case BT_NOTIFY_COMMON_ACL_DISCONNECTED:
        {
            bt_notify_device_base_info_t *info = (bt_notify_device_base_info_t *)data;

            LOG_I("disconnected(0x%.2x:%.2x:%.2x:%.2x:%.2x:%.2x) res %d",
                  info->mac.addr[5], info->mac.addr[4], info->mac.addr[3],
                  info->mac.addr[2], info->mac.addr[1], info->mac.addr[0], info->res);
            g_pan.bt_connected = RT_FALSE;
            g_pan.pan_connected = RT_FALSE;
            btpan_reset_mailbox();
            btpan_stop_timer();

            /* 恢复可被连接，并对已配对设备启动自动重连 */
            if (g_pan.enabled && g_pan.stack_ready && btpan_has_peer_addr())
            {
                bt_interface_set_scan_mode(TRUE, TRUE);
                btpan_start_reconnect();
            }

            btpan_notify_state();
            break;
        }

        case BT_NOTIFY_COMMON_ENCRYPTION:
        {
            bt_notify_device_mac_t *mac = (bt_notify_device_mac_t *)data;

            LOG_I("Encryption completed");
            g_pan.bd_addr = *mac;
            should_connect_pan = RT_TRUE;
            break;
        }

        case BT_NOTIFY_COMMON_PAIR_IND:
        {
            bt_notify_device_base_info_t *info = (bt_notify_device_base_info_t *)data;

            LOG_I("Pairing completed %d", info->res);
            if (info->res == BTS2_SUCC)
            {
                g_pan.bd_addr = info->mac;
                should_connect_pan = RT_TRUE;
            }
            break;
        }

        case BT_NOTIFY_COMMON_KEY_MISSING:
        {
            bt_notify_device_base_info_t *info = (bt_notify_device_base_info_t *)data;

            LOG_I("Key missing %d", info->res);
            memset(&g_pan.bd_addr, 0xFF, sizeof(g_pan.bd_addr));
            bt_cm_delete_bonded_devs_and_linkkey(info->mac.addr);
            break;
        }

        default:
            break;
        }

        /* 配对或加密完成后，启动延迟定时器发起 PAN 连接。 */
        if (should_connect_pan)
        {
            LOG_I("bd addr 0x%.2x:%.2x:%.2x:%.2x:%.2x:%.2x",
                  g_pan.bd_addr.addr[5], g_pan.bd_addr.addr[4],
                  g_pan.bd_addr.addr[3], g_pan.bd_addr.addr[2],
                  g_pan.bd_addr.addr[1], g_pan.bd_addr.addr[0]);
            g_pan.bt_connected = RT_TRUE;
            btpan_stop_reconnect(); /* 链路已恢复，停止重连轮询 */
            btpan_notify_state();
            btpan_start_timer();
        }
    }
    /* ---- PAN profile 事件 ---- */
    else if (type == BT_NOTIFY_PAN)
    {
        bt_notify_profile_state_info_t *info = (bt_notify_profile_state_info_t *)data;
        int res = (info != RT_NULL) ? info->res : -1;

        switch (event_id)
        {
        case BT_NOTIFY_PAN_PROFILE_CONNECTED:
            LOG_I("pan connect successed (res=%d)", res);
            btpan_stop_timer();
            btpan_stop_reconnect();
            g_pan.connect_pending = RT_FALSE;
            g_pan.pan_connected = RT_TRUE;
            btpan_notify_state();
            break;

        case BT_NOTIFY_PAN_PROFILE_DISCONNECTED:
            LOG_I("pan disconnect with remote device (res=%d)", res);
            g_pan.pan_connected = RT_FALSE;
            btpan_reset_mailbox();
            btpan_notify_state();
            break;

        default:
            break;
        }
    }

    return 0;
}

/*---------------------------------------------------------------------------*/
/* 本机蓝牙 MAC 地址（NVDS 持久化，重启后生效）
 *
 * 开机协议栈就绪后将下面这组自定义地址写入 NVDS；
 * 写入后需【重启设备】才会生效（协议栈启动时才重新读取）。
 *---------------------------------------------------------------------------*/
/* 自定义本机蓝牙 MAC（直接修改数组即可；显示顺序：下标 0 为显示首字节，
   即 02:4C:56:45:50:44） */
static const uint8_t s_custom_bd_addr[6] = { 0x02, 0x4C, 0x56, 0x45, 0x50, 0x44 };

/* NVDS 地址写入接口（声明见 bf0_sibles_nvds.h；实现位于 service/common/bf0_bt_nvds.c） */
extern uint8_t ble_nvds_update_address(bd_addr_t *addr, ble_common_update_type_t u_type, uint8_t is_flush);

void btpan_get_local_addr(char *buf, rt_size_t len)
{
    bd_addr_t addr;

    if (buf == RT_NULL || len < 18)
        return;

    buf[0] = '\0';

    if (ble_get_public_address(&addr) != 0)
        return;

    rt_snprintf(buf, len, "%02X:%02X:%02X:%02X:%02X:%02X",
                addr.addr[0], addr.addr[1], addr.addr[2],
                addr.addr[3], addr.addr[4], addr.addr[5]);
}

/*---------------------------------------------------------------------------*/
/* 工作线程 */
/*---------------------------------------------------------------------------*/
static void btpan_stack_ready(void)
{
    char mac[18];
    bd_addr_t addr;

    g_pan.stack_ready = RT_TRUE;
    LOG_I("BT/BLE stack and profile ready");

    /* 写入自定义本机蓝牙 MAC（NVDS 持久化，重启后生效） */
    memcpy(addr.addr, s_custom_bd_addr, sizeof(s_custom_bd_addr));
    if (ble_nvds_update_address(&addr, BLE_UPDATE_ALWAYS, 1) != 0)
        LOG_E("set custom bd addr failed");

    btpan_get_local_addr(mac, sizeof(mac));
    LOG_I("local bd addr: %s", mac);

    if (g_pan.local_name[0] != '\0')
        bt_interface_set_local_name(strlen(g_pan.local_name), g_pan.local_name);

    bt_interface_set_scan_mode(g_pan.enabled, g_pan.enabled);
    btpan_notify_state();
}

static void btpan_worker_entry(void *parameter)
{
    rt_uint32_t value = 0;

    (void)parameter;

    /* 首次等待协议栈 ready，并在 ready 后设置本地蓝牙名称。 */
    if (RT_EOK == rt_mb_recv(g_pan.mailbox, &value, 8000) && value == PAN_MSG_STACK_READY)
    {
        btpan_stack_ready();
    }
    else
    {
        LOG_I("BT/BLE stack and profile init failed");
    }

    /* 继续接收迟到的协议栈 ready 事件及后续业务事件。 */
    while (1)
    {
        if (rt_mb_recv(g_pan.mailbox, &value, RT_WAITING_FOREVER) != RT_EOK)
            continue;

        switch (value)
        {
        case PAN_MSG_STACK_READY:
            btpan_stack_ready();
            break;

        case PAN_MSG_CONNECT_PAN:
            g_pan.connect_pending = RT_FALSE;
            if (g_pan.bt_connected)
                bt_interface_conn_ext((char *)&g_pan.bd_addr, BT_PROFILE_PAN);
            break;

        default:
            break;
        }
    }
}

/*---------------------------------------------------------------------------*/
/* 对外接口 */
/*---------------------------------------------------------------------------*/
/**
 * @brief 返回设备蓝牙 Class of Device，用于声明网络设备能力。
 */
uint32_t bt_get_class_of_device(void)
{
    return (uint32_t)BT_SRVCLS_NETWORK | BT_COMPCLS_PALMSIZED;
}

rt_err_t btpan_init(const char *device_name)
{
    if (g_pan.initialized)
        return RT_EOK;

    memset(&g_pan, 0, sizeof(g_pan));

    if (device_name == RT_NULL || device_name[0] == '\0')
        device_name = "sifli_pan";

    rt_strncpy(g_pan.local_name, device_name, sizeof(g_pan.local_name) - 1);
    g_pan.enabled = RT_TRUE;
    g_pan.last_state = BTPAN_STATE_OFF;

#ifdef BSP_BT_CONNECTION_MANAGER
    /* 将 PAN 加入“手机类设备”的目标 profile 集合：
       手机（重新）连接后，连接管理器会在合适时机自动拉起 PAN 连接 */
    bt_cm_set_profile_target(BT_CM_PAN, BT_LINK_PHONE, 1);
#endif

    LOG_I("btpan init: %s", g_pan.local_name);

    g_pan.mailbox = rt_mb_create("bt_app", 4, RT_IPC_FLAG_FIFO);
    if (g_pan.mailbox == RT_NULL)
        return -RT_ENOMEM;

    g_pan.worker = rt_thread_create("pan_worker",
                                    btpan_worker_entry,
                                    RT_NULL,
                                    PAN_THREAD_STACK_SIZE,
                                    PAN_THREAD_PRIORITY,
                                    PAN_THREAD_TICK);
    if (g_pan.worker == RT_NULL)
    {
        rt_mb_delete(g_pan.mailbox);
        g_pan.mailbox = RT_NULL;
        return -RT_ENOMEM;
    }

    g_pan.initialized = RT_TRUE;

    bt_interface_register_bt_event_notify_callback(btpan_bt_event_handle);
    rt_thread_startup(g_pan.worker);

    btpan_notify_state(); /* OFF -> INITIALIZING */
    sifli_ble_enable();

    return RT_EOK;
}

rt_err_t btpan_enable(bool enable)
{
    if (!g_pan.initialized)
        return -RT_ERROR;

    if (!!enable == !!g_pan.enabled)
        return RT_EOK;

    if (enable)
    {
        g_pan.enabled = RT_TRUE;
        if (g_pan.stack_ready)
        {
            if (g_pan.local_name[0] != '\0')
                bt_interface_set_local_name(strlen(g_pan.local_name), g_pan.local_name);
            bt_interface_set_scan_mode(TRUE, TRUE);
        }
    }
    else
    {
        g_pan.enabled = RT_FALSE;

        if (g_pan.stack_ready)
            bt_interface_set_scan_mode(FALSE, FALSE);

        if (btpan_has_peer_addr())
        {
            if (g_pan.pan_connected)
                bt_interface_disc_ext((unsigned char *)&g_pan.bd_addr, BT_PROFILE_PAN);

            if (g_pan.bt_connected)
                bt_interface_disconnect_req((unsigned char *)&g_pan.bd_addr);
        }

        g_pan.bt_connected = RT_FALSE;
        g_pan.pan_connected = RT_FALSE;
        btpan_reset_mailbox();
        btpan_stop_timer();
        btpan_stop_reconnect();
    }

    btpan_notify_state();
    return RT_EOK;
}

rt_err_t btpan_request_connect(void)
{
    if (!g_pan.initialized || !g_pan.bt_connected)
        return -RT_ERROR;

    if (g_pan.pan_connected || g_pan.connect_pending)
        return RT_EOK;

    g_pan.connect_pending = RT_TRUE;
    if (btpan_post(PAN_MSG_CONNECT_PAN) != RT_EOK)
    {
        g_pan.connect_pending = RT_FALSE;
        return -RT_ERROR;
    }

    return RT_EOK;
}

btpan_state_t btpan_get_state(void)
{
    return btpan_current_state();
}

bool btpan_is_ready(void)
{
    return g_pan.stack_ready ? true : false;
}

bool btpan_is_connected(void)
{
    return g_pan.bt_connected ? true : false;
}

bool btpan_is_network_ready(void)
{
    return g_pan.pan_connected ? true : false;
}

void btpan_set_event_cb(btpan_event_cb_t cb)
{
    g_pan.event_cb = cb;
}
