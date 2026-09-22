/*
 * SPDX-FileCopyrightText: 2024-2025 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/**
 * @file service_cmd.c
 * @brief 数据服务调试命令（msh）：通过串口直接调用服务接口，验证返回数据
 *
 * 用法（串口 msh，风格参考 rt_bt_app_cmd.c）：
 *   svc                                  列出所有服务
 *   svc weather status                   打印当前天气快照
 *   svc weather refresh                  请求刷新（等待完成后打印）
 *   svc weather city [id]                查看/设置城市（beijing/shanghai/nanjing/...）
 *   svc pan status|on|off|connect        蓝牙 PAN 状态与控制
 *   svc bookshelf list                   扫描并打印书库
 *   svc reader open <path>               打开书籍
 *   svc reader info                      当前书籍信息
 *   svc reader read [offset] [len]       按偏移读取一段文本
 *   svc reader next [len]                从当前进度读取并前进
 *   svc reader seek <offset>             设置阅读位置
 *   svc reader close                     关闭书籍
 */
#include <rtthread.h>
#include <stdlib.h>
#include <string.h>

#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"

#include "weather.h"
#include "bt_pan.h"
#include "bookshelf.h"
#include "reader.h"

#define DBG_TAG "svc.cmd"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/*---------------------------------------------------------------------------*/
/* 小工具 */
/*---------------------------------------------------------------------------*/
static const char *weather_state_name(weather_state_t st)
{
    switch (st)
    {
    case WEATHER_STATE_IDLE:       return "IDLE";
    case WEATHER_STATE_REFRESHING: return "REFRESHING";
    case WEATHER_STATE_UPDATED:    return "UPDATED";
    case WEATHER_STATE_CACHED:     return "CACHED";
    case WEATHER_STATE_FAILED:     return "FAILED";
    default:                       return "?";
    }
}

static const char *pan_state_name(btpan_state_t st)
{
    switch (st)
    {
    case BTPAN_STATE_OFF:           return "OFF";
    case BTPAN_STATE_INITIALIZING:  return "INITIALIZING";
    case BTPAN_STATE_READY:         return "READY (waiting phone)";
    case BTPAN_STATE_CONNECTED:     return "CONNECTED (network pending)";
    case BTPAN_STATE_NETWORK_READY: return "NETWORK_READY (online)";
    default:                        return "?";
    }
}

/*---------------------------------------------------------------------------*/
/* weather 子命令 */
/*---------------------------------------------------------------------------*/
static void print_weather_snapshot(void)
{
    weather_info_t w;
    int i;

    weather_get_info(&w);

    rt_kprintf("========== weather snapshot ==========\n");
    rt_kprintf("valid   : %d\n", w.valid);
    rt_kprintf("state   : %s\n", weather_state_name(w.state));
    rt_kprintf("status  : %s\n", w.status);
    rt_kprintf("city    : %s\n", w.city);
    rt_kprintf("update  : %s\n", w.update_time);
    rt_kprintf("now     : %s (code=%d) %dC  feels %dC  H:%d L:%d\n",
               w.text, w.code, w.temperature, w.feels_like, w.high, w.low);
    rt_kprintf("detail  : humidity %d%% | wind %s L%d %dkm/h | vis %dkm | press %dhPa | cloud %d%%\n",
               w.humidity, w.wind_dir, w.wind_scale, w.wind_speed,
               w.visibility, w.pressure, w.cloud);
    rt_kprintf("air     : aqi %d %s | sunrise %s | sunset %s\n",
               w.aqi, w.aqi_category, w.sunrise, w.sunset);

    for (i = 0; i < w.forecast_count; i++)
    {
        const weather_forecast_t *f = &w.forecast[i];
        rt_kprintf("forecast%d: %s %s (code=%d) %d~%dC  wind %s L%d\n",
                   i, f->date, f->text, f->code, f->low, f->high, f->wind_dir, f->wind_scale);
    }
    rt_kprintf("======================================\n");
}

static void weather_wait_done(void)
{
    int i;
    int seen_refreshing = 0;

    for (i = 0; i < 150; i++) /* 最多 30s */
    {
        weather_state_t st = weather_get_state();

        if (st == WEATHER_STATE_REFRESHING)
        {
            seen_refreshing = 1;
        }
        else if (seen_refreshing)
        {
            return; /* 已离开刷新态 -> 完成 */
        }
        else if (i >= 10)
        {
            return; /* 2s 内未观察到刷新态，视为瞬时完成 */
        }

        rt_thread_mdelay(200);
    }
}

static void cmd_weather_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    print_weather_snapshot();
}

static void cmd_weather_refresh(int argc, char **argv)
{
    rt_err_t ret;

    (void)argc;
    (void)argv;

    ret = weather_request_refresh();
    if (ret != RT_EOK)
    {
        rt_kprintf("request failed (%d), service not ready?\n", ret);
        return;
    }

    rt_kprintf("refresh requested, waiting for result...\n");
    weather_wait_done();
    print_weather_snapshot();
}

static void cmd_weather_city(int argc, char **argv)
{
    if (argc < 2)
    {
        const weather_city_t *list;
        int count = 0;
        int i;

        rt_kprintf("current city: %s\n", weather_get_city());
        list = weather_get_city_list(&count);
        rt_kprintf("available   :");
        for (i = 0; i < count; i++)
            rt_kprintf(" %s", list[i].id);
        rt_kprintf("\n");
        return;
    }

    if (weather_set_city(argv[1]) != RT_EOK)
        rt_kprintf("invalid city id \"%s\" (use \"svc weather city\" to list)\n", argv[1]);
    else
        rt_kprintf("city set to: %s\n", weather_get_city());
}

/*---------------------------------------------------------------------------*/
/* pan 子命令 */
/*---------------------------------------------------------------------------*/
static void cmd_pan_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    rt_kprintf("state          : %s\n", pan_state_name(btpan_get_state()));
    rt_kprintf("stack ready    : %s\n", btpan_is_ready() ? "yes" : "no");
    rt_kprintf("phone connected: %s\n", btpan_is_connected() ? "yes" : "no");
    rt_kprintf("network ready  : %s\n", btpan_is_network_ready() ? "yes" : "no");
}

static void cmd_pan_on(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    rt_kprintf("btpan_enable(true): %d\n", btpan_enable(true));
}

static void cmd_pan_off(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    rt_kprintf("btpan_enable(false): %d\n", btpan_enable(false));
}

static void cmd_pan_connect(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    rt_kprintf("btpan_request_connect: %d\n", btpan_request_connect());
}

static void cmd_pan_ip(int argc, char **argv)
{
    struct netif *netif;
    char buf[IP4ADDR_STRLEN_MAX];
    char buf0[IP4ADDR_STRLEN_MAX];
    char buf1[IP4ADDR_STRLEN_MAX];

    (void)argc;
    (void)argv;

    /* bt_lwip 注册的 PAN 网卡名为 "b0" */
    netif = netif_find("b0");
    if (netif == RT_NULL)
    {
        rt_kprintf("PAN netif (b0) not found - bluetooth PAN not initialized\n");
        return;
    }

    rt_kprintf("netif     : b0\n");
    rt_kprintf("link      : %s\n", netif_is_link_up(netif) ? "up" : "down");
    rt_kprintf("dhcp      : %s\n", dhcp_supplied_address(netif) ? "leased" : "no lease");
    rt_kprintf("IP        : %s\n", ip4addr_ntoa_r(netif_ip4_addr(netif), buf, sizeof(buf)));
    rt_kprintf("Netmask   : %s\n", ip4addr_ntoa_r(netif_ip4_netmask(netif), buf, sizeof(buf)));
    rt_kprintf("Gateway   : %s\n", ip4addr_ntoa_r(netif_ip4_gw(netif), buf, sizeof(buf)));
    rt_kprintf("DNS       : %s / %s\n",
               ipaddr_ntoa_r(dns_getserver(0), buf0, sizeof(buf0)),
               ipaddr_ntoa_r(dns_getserver(1), buf1, sizeof(buf1)));
}

/*---------------------------------------------------------------------------*/
/* bookshelf 子命令 */
/*---------------------------------------------------------------------------*/
static void cmd_bs_list(int argc, char **argv)
{
    bookshelf_item_t item;
    int i;
    int n;

    (void)argc;
    (void)argv;

    n = bookshelf_refresh();
    rt_kprintf("bookshelf \"%s\": %d book(s)\n", bookshelf_dir(), n);

    for (i = 0; i < n; i++)
    {
        if (bookshelf_get(i, &item))
        {
            rt_kprintf("  [%d] %-32s %u bytes  progress %d%%\n",
                       i, item.name, (unsigned)item.size, item.progress);
        }
    }
}

/*---------------------------------------------------------------------------*/
/* reader 子命令 */
/*---------------------------------------------------------------------------*/
static int reader_get_len_arg(int argc, char **argv, int arg_index)
{
    int len = 512;

    if (argc > arg_index)
    {
        len = atoi(argv[arg_index]);
        if (len < 16)
            len = 16;
        if (len > 2048)
            len = 2048;
    }

    return len;
}

static void cmd_rd_open(int argc, char **argv)
{
    rt_err_t ret;

    if (argc < 2)
    {
        rt_kprintf("usage: svc reader open <path>\n");
        return;
    }

    ret = reader_open(argv[1]);
    rt_kprintf("open \"%s\": %s\n", argv[1], (ret == RT_EOK) ? "ok" : "FAILED");
}

static void cmd_rd_info(int argc, char **argv)
{
    reader_info_t info;

    (void)argc;
    (void)argv;

    if (!reader_get_info(&info))
    {
        rt_kprintf("no book opened (svc reader open <path>)\n");
        return;
    }

    rt_kprintf("title    : %s\n", info.title);
    rt_kprintf("path     : %s\n", info.path);
    rt_kprintf("size     : %u bytes\n", (unsigned)info.file_size);
    rt_kprintf("encoding : %s\n", info.encoding);
    rt_kprintf("position : %u (%d%%)\n", (unsigned)info.position, reader_get_percent());
}

static void cmd_rd_read(int argc, char **argv)
{
    uint32_t offset;
    uint32_t next = 0;
    int len;
    char *buf;
    int n;

    if (!reader_is_open())
    {
        rt_kprintf("no book opened (svc reader open <path>)\n");
        return;
    }

    offset = (argc > 1) ? (uint32_t)strtoul(argv[1], RT_NULL, 0) : reader_get_position();
    len = reader_get_len_arg(argc, argv, 2);

    buf = rt_malloc((rt_size_t)len + 1);
    if (buf == RT_NULL)
    {
        rt_kprintf("no memory\n");
        return;
    }

    n = reader_read_text(offset, buf, (rt_size_t)len + 1, &next);
    if (n < 0)
    {
        rt_kprintf("read failed (%d)\n", n);
    }
    else
    {
        rt_kprintf("---- read @%u, %d bytes, next %u ----\n",
                   (unsigned)offset, n, (unsigned)next);
        rt_kprintf("%s\n", buf);
        rt_kprintf("---- end ----\n");
    }

    rt_free(buf);
}

static void cmd_rd_next(int argc, char **argv)
{
    int len;
    char *buf;
    int n;

    if (!reader_is_open())
    {
        rt_kprintf("no book opened (svc reader open <path>)\n");
        return;
    }

    len = reader_get_len_arg(argc, argv, 1);

    buf = rt_malloc((rt_size_t)len + 1);
    if (buf == RT_NULL)
    {
        rt_kprintf("no memory\n");
        return;
    }

    n = reader_read_next(buf, (rt_size_t)len + 1);
    if (n < 0)
    {
        rt_kprintf("read failed (%d)\n", n);
    }
    else
    {
        rt_kprintf("---- read %d bytes, position now %u (%d%%) ----\n",
                   n, (unsigned)reader_get_position(), reader_get_percent());
        rt_kprintf("%s\n", buf);
        rt_kprintf("---- end ----\n");
    }

    rt_free(buf);
}

static void cmd_rd_seek(int argc, char **argv)
{
    if (argc < 2)
    {
        rt_kprintf("usage: svc reader seek <offset>\n");
        return;
    }

    reader_set_position((uint32_t)strtoul(argv[1], RT_NULL, 0));
    rt_kprintf("position: %u (%d%%)\n", (unsigned)reader_get_position(), reader_get_percent());
}

static void cmd_rd_close(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    reader_close();
    rt_kprintf("closed\n");
}

/*---------------------------------------------------------------------------*/
/* 命令表（风格参考 rt_bt_app_cmd.c） */
/*---------------------------------------------------------------------------*/
typedef struct
{
    const char *name;
    const char *usage;
    void (*handler)(int argc, char **argv);
} svc_cmd_t;

typedef struct
{
    const char *name;
    const svc_cmd_t *cmds;
    rt_size_t cmd_num;
} svc_group_t;

static const svc_cmd_t weather_cmds[] =
{
    { "status",  "print weather snapshot",           cmd_weather_status },
    { "refresh", "request refresh and wait result",  cmd_weather_refresh },
    { "city",    "show/set city: city [id]",         cmd_weather_city },
};

static const svc_cmd_t pan_cmds[] =
{
    { "status",  "print pan state",                  cmd_pan_status },
    { "on",      "enable bluetooth pan service",     cmd_pan_on },
    { "off",     "disable bluetooth pan service",    cmd_pan_off },
    { "connect", "request pan connection",           cmd_pan_connect },
    { "ip",      "show PAN netif IP address (DHCP)", cmd_pan_ip },
};

static const svc_cmd_t bookshelf_cmds[] =
{
    { "list",    "scan and list books in /book",     cmd_bs_list },
};

static const svc_cmd_t reader_cmds[] =
{
    { "open",    "open book: open <path>",           cmd_rd_open },
    { "info",    "current book info",                cmd_rd_info },
    { "read",    "read: read [offset] [len]",        cmd_rd_read },
    { "next",    "read from position: next [len]",   cmd_rd_next },
    { "seek",    "set position: seek <offset>",      cmd_rd_seek },
    { "close",   "close book",                       cmd_rd_close },
};

static const svc_group_t g_groups[] =
{
    { "weather",   weather_cmds,   sizeof(weather_cmds) / sizeof(weather_cmds[0]) },
    { "pan",       pan_cmds,       sizeof(pan_cmds) / sizeof(pan_cmds[0]) },
    { "bookshelf", bookshelf_cmds, sizeof(bookshelf_cmds) / sizeof(bookshelf_cmds[0]) },
    { "reader",    reader_cmds,    sizeof(reader_cmds) / sizeof(reader_cmds[0]) },
};

#define SVC_GROUP_COUNT (sizeof(g_groups) / sizeof(g_groups[0]))

/**
 * @brief msh 入口：svc <service> <sub-command> [args]
 */
static void svc(int argc, char **argv)
{
    const svc_group_t *grp = RT_NULL;
    const svc_cmd_t *cmd = RT_NULL;
    rt_size_t i;

    if (argc < 2)
    {
        rt_kprintf("data service debug commands:\n");
        for (i = 0; i < SVC_GROUP_COUNT; i++)
            rt_kprintf("  svc %-10s (%u sub-commands)\n",
                       g_groups[i].name, (unsigned)g_groups[i].cmd_num);
        rt_kprintf("use \"svc <service>\" to list sub-commands\n");
        return;
    }

    for (i = 0; i < SVC_GROUP_COUNT; i++)
    {
        if (rt_strcmp(g_groups[i].name, argv[1]) == 0)
        {
            grp = &g_groups[i];
            break;
        }
    }

    if (grp == RT_NULL)
    {
        rt_kprintf("svc: unknown service \"%s\" (try \"svc\")\n", argv[1]);
        return;
    }

    if (argc < 3)
    {
        rt_kprintf("usage: svc %s <sub-command> [args]\n", grp->name);
        for (i = 0; i < grp->cmd_num; i++)
            rt_kprintf("  %-10s %s\n", grp->cmds[i].name, grp->cmds[i].usage);
        return;
    }

    for (i = 0; i < grp->cmd_num; i++)
    {
        if (rt_strcmp(grp->cmds[i].name, argv[2]) == 0)
        {
            cmd = &grp->cmds[i];
            break;
        }
    }

    if (cmd == RT_NULL)
    {
        rt_kprintf("svc %s: unknown sub-command \"%s\"\n", grp->name, argv[2]);
        return;
    }

    cmd->handler(argc - 2, argv + 2);
}
MSH_CMD_EXPORT(svc, data service debug: svc <service> <cmd> [args]);
