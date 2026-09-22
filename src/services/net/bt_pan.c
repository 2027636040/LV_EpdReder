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
 * 注意：对外符号统一使用 btpan_ 前缀（SDK 已占用 bt_pan_ 前缀）。
 */
#include "bt_pan.h"

#include <string.h>

#include "bts2_app_inc.h"
#include "ble_connection_manager.h"
#include "bt_connection_manager.h"
#include "ulog.h"

/*---------------------------------------------------------------------------*/
/* 配置常量 */
/*---------------------------------------------------------------------------*/
#define PAN_TIMER_MS           3000
#define PAN_THREAD_STACK_SIZE  4096
#define PAN_THREAD_PRIORITY    20
#define PAN_THREAD_TICK        20
#define PAN_LOCAL_NAME_MAX     32

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
            btpan_notify_state();
            btpan_start_timer();
        }
    }
    /* ---- PAN profile 事件 ---- */
    else if (type == BT_NOTIFY_PAN)
    {
        switch (event_id)
        {
        case BT_NOTIFY_PAN_PROFILE_CONNECTED:
            LOG_I("pan connect successed");
            btpan_stop_timer();
            g_pan.connect_pending = RT_FALSE;
            g_pan.pan_connected = RT_TRUE;
            btpan_notify_state();
            break;

        case BT_NOTIFY_PAN_PROFILE_DISCONNECTED:
            LOG_I("pan disconnect with remote device");
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
/* 工作线程 */
/*---------------------------------------------------------------------------*/
static void btpan_worker_entry(void *parameter)
{
    rt_uint32_t value = 0;

    (void)parameter;

    /* 首次等待协议栈 ready，并在 ready 后设置本地蓝牙名称。 */
    if (RT_EOK == rt_mb_recv(g_pan.mailbox, &value, 8000) && value == PAN_MSG_STACK_READY)
    {
        g_pan.stack_ready = RT_TRUE;
        LOG_I("BT/BLE stack and profile ready");
        btpan_notify_state();
    }
    else
    {
        LOG_I("BT/BLE stack and profile init failed");
    }

    if (g_pan.local_name[0] != '\0')
        bt_interface_set_local_name(strlen(g_pan.local_name), g_pan.local_name);

    bt_interface_set_scan_mode(TRUE, TRUE);

    /* 后续循环只处理业务事件。 */
    while (1)
    {
        if (rt_mb_recv(g_pan.mailbox, &value, RT_WAITING_FOREVER) != RT_EOK)
            continue;

        switch (value)
        {
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
