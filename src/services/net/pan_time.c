/* SPDX-License-Identifier: Apache-2.0 */
#include "pan_time.h"
#include <rthw.h>
#include <rtdevice.h>
#include <lwip/api.h>
#include <lwip/tcpip.h>
#include <lwip/netif.h>
#include <lwip/sockets.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#define DBG_TAG "pan.time"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define NTP_PORT 123
#define NTP_TIMEOUT_MS 5000
#define SYNC_RETRY_MS 30000
#define SYNC_ATTEMPTS 3u
#define NTP_EPOCH_DELTA 2208988800ULL
#define MIN_UTC_TIME 1704067200ULL /* 2024-01-01 */
#define MAX_UTC_TIME 4102444800ULL /* 2100-01-01, exclusive */

static struct rt_semaphore wakeup, net_checked;
static rt_thread_t worker;
static bool link_up;
static uint32_t link_generation;
/* Written on tcpip_thread, read only after net_checked is signalled. */
static bool net_ready;
static ip4_addr_t local_address;

static bool link_snapshot(uint32_t *generation)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool connected = link_up;
    *generation = link_generation;
    rt_hw_interrupt_enable(level);
    return connected;
}

static bool same_link(uint32_t generation)
{
    uint32_t current;
    return link_snapshot(&current) && current == generation;
}

static void check_netif(void *context)
{
    struct netif *netif;
    (void)context;
    net_ready = false;
    NETIF_FOREACH(netif)
    {
        if (netif->name[0] == 'b' && netif_is_up(netif) && netif_is_link_up(netif) &&
            !ip4_addr_isany_val(*netif_ip4_addr(netif)) && !ip4_addr_isany_val(*netif_ip4_gw(netif)))
        {
            local_address = *netif_ip4_addr(netif);
            netif_set_default(netif);
            net_ready = true;
            break;
        }
    }
    rt_sem_release(&net_checked);
}

static bool network_ready(void)
{
    if (tcpip_callback(check_netif, RT_NULL) != ERR_OK) return false;
    rt_sem_take(&net_checked, RT_WAITING_FOREVER);
    return net_ready;
}

/* A private SNTP transaction avoids the SDK client's shared packet/netif state. */
static rt_err_t request_time(uint32_t generation, time_t *utc)
{
    ip_addr_t server_ip;
    struct sockaddr_in address;
    uint32_t request[12] = {0}, response[12];
    rt_err_t result = -RT_ERROR;
    const char *stage = "dns";
    int fd = -1, detail = 0, nonblocking = 1;
    rt_tick_t start;

    detail = netconn_gethostbyname(NETUTILS_NTP_HOSTNAME, &server_ip);
    if (detail != ERR_OK) goto done;
    if (!same_link(generation)) goto done;

    stage = "socket";
    fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) { detail = errno; goto done; }
    if (ioctlsocket(fd, FIONBIO, &nonblocking) != 0) { detail = errno; goto done; }

    /* Pin the request to this PAN address, without retaining a netif pointer. */
    stage = "bind";
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = ip4_addr_get_u32(&local_address);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0)
    { detail = errno; goto done; }

    stage = "connect";
    address.sin_port = htons(NTP_PORT);
    address.sin_addr.s_addr = ip4_addr_get_u32(ip_2_ip4(&server_ip));
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0)
    { detail = errno; goto done; }

    /* VN=4, mode=client. The server must echo this transaction's transmit stamp. */
    request[0] = htonl(0x23000000u);
    request[10] = htonl((uint32_t)(NTP_EPOCH_DELTA + MIN_UTC_TIME + generation));
    request[11] = htonl(rt_tick_get());
    stage = "send";
    if (send(fd, request, sizeof(request), 0) != (int)sizeof(request))
    { detail = errno; goto done; }

    stage = "receive";
    result = -RT_ETIMEOUT;
    start = rt_tick_get();
    while (same_link(generation) &&
           (rt_tick_t)(rt_tick_get() - start) < rt_tick_from_millisecond(NTP_TIMEOUT_MS))
    {
        fd_set readfds;
        struct timeval timeout = {0, 200000};
        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);
        int count = select(fd + 1, &readfds, RT_NULL, RT_NULL, &timeout);
        if (count < 0)
        {
            if (errno == EINTR) continue;
            detail = errno;
            result = -RT_ERROR;
            break;
        }
        if (!count) continue;
        count = recv(fd, response, sizeof(response), 0);
        if (count < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            detail = errno;
            result = -RT_ERROR;
            break;
        }
        if (count < (int)sizeof(response)) continue;
        uint32_t header = ntohl(response[0]);
        unsigned version = (header >> 27) & 7;
        unsigned stratum = (header >> 16) & 255;
        if ((version != 3 && version != 4) || ((header >> 24) & 7) != 4 ||
            response[6] != request[10] || response[7] != request[11]) continue;
        /* Stratum zero is a server denial/rate-limit response; stop this session. */
        if (!stratum)
        {
            stage = "server-denied";
            result = -RT_EBUSY;
            break;
        }
        if ((header >> 30) == 3 || stratum > 15 || (!response[10] && !response[11])) continue;

        uint64_t seconds = ntohl(response[10]);
        if (seconds < NTP_EPOCH_DELTA) seconds += 1ULL << 32; /* NTP era rollover in 2036. */
        seconds -= NTP_EPOCH_DELTA;
        if (seconds < MIN_UTC_TIME || seconds >= MAX_UTC_TIME) continue;
        *utc = (time_t)seconds;
        result = RT_EOK;
        break;
    }

done:
    if (fd >= 0) closesocket(fd);
    if (result != RT_EOK && same_link(generation))
        LOG_W("NTP failed: stage=%s result=%d detail=%d", stage, result, detail);
    return result;
}

static bool update_rtc(time_t utc)
{
    time_t timestamp = utc;
#if RT_VER_NUM <= 0x40003
    /* This SDK stores local wall-clock seconds; newlib has no configured TZ. */
    timestamp += NETUTILS_NTP_TIMEZONE * 3600;
#endif
    rt_device_t rtc = rt_device_find("rtc");
    if (!rtc || rt_device_control(rtc, RT_DEVICE_CTRL_RTC_SET_TIME, &timestamp) != RT_EOK)
    {
        LOG_W("RTC write failed");
        return false;
    }
    struct tm calendar;
    char text[32];
    if (localtime_r(&timestamp, &calendar) && strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &calendar))
        LOG_I("RTC synchronized: %s", text);
    return true;
}

static void time_worker(void *parameter)
{
    uint32_t generation = 0;
    unsigned attempts = 0;
    bool synced = false;
    rt_tick_t last_attempt = 0;
    (void)parameter;

    while (1)
    {
        uint32_t current;
        bool connected = link_snapshot(&current);
        if (current != generation)
        {
            generation = current;
            attempts = 0;
            synced = false;
        }
        bool active = connected && !synced && attempts < SYNC_ATTEMPTS;
        if (active && (!attempts || (rt_tick_t)(rt_tick_get() - last_attempt) >=
                                   rt_tick_from_millisecond(SYNC_RETRY_MS)) && network_ready() &&
            same_link(generation))
        {
            time_t utc;
            attempts++;
            LOG_I("PAN network ready, NTP sync %u/%u: %s", attempts, SYNC_ATTEMPTS, NETUTILS_NTP_HOSTNAME);
            rt_err_t result = request_time(generation, &utc);
            if (same_link(generation))
            {
                if (result == RT_EOK) synced = update_rtc(utc);
                if (result == -RT_EBUSY) attempts = SYNC_ATTEMPTS;
                if (!synced && attempts == SYNC_ATTEMPTS)
                    LOG_W("Time sync stopped; retry on next PAN connection");
            }
            last_attempt = rt_tick_get();
        }
        rt_sem_take(&wakeup, active ? rt_tick_from_millisecond(1000) : RT_WAITING_FOREVER);
    }
}

rt_err_t pan_time_init(void)
{
    if (worker) return RT_EOK;
    rt_err_t result = rt_sem_init(&wakeup, "time_w", 0, RT_IPC_FLAG_FIFO);
    if (result != RT_EOK) return result;
    result = rt_sem_init(&net_checked, "time_n", 0, RT_IPC_FLAG_FIFO);
    if (result != RT_EOK) { rt_sem_detach(&wakeup); return result; }
    worker = rt_thread_create("pan_time", time_worker, RT_NULL, 4096, 23, 10);
    if (worker)
    {
        result = rt_thread_startup(worker);
        if (result == RT_EOK) return result;
        rt_thread_delete(worker);
        worker = RT_NULL;
    }
    else result = -RT_ENOMEM;
    rt_sem_detach(&net_checked);
    rt_sem_detach(&wakeup);
    return result;
}

void pan_time_set_link(bool connected)
{
    if (!worker) return;
    rt_base_t level = rt_hw_interrupt_disable();
    bool changed = connected != link_up;
    if (changed)
    {
        link_up = connected;
        link_generation++;
    }
    rt_hw_interrupt_enable(level);
    if (changed) rt_sem_release(&wakeup);
}
