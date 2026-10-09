#include "network.h"
#include "bt_pan.h"
#include <lwip/netif.h>
#include <lwip/tcpip.h>
#include <lwip/dns.h>
#include <rtm.h>
#include <string.h>

typedef struct network_subscription
{
    struct network_subscription *next;
    rt_event_t event;
    uint32_t bits, token;
} network_subscription_t;
static struct rt_event wakeup;
static struct rt_mutex lock;
static struct rt_semaphore sampled;
static rt_thread_t worker;
static network_snapshot_t state, candidate;
static network_source_t selected;
static char interfaces[3][NETIF_NAMESIZE] = {"", "b0", ""};
static network_subscription_t *subscribers;
static uint32_t next_token;
static ip_addr_t resolvers[3][DNS_MAX_SERVERS];
static bool dns_valid[3];
static uint32_t epochs[3], sampled_epoch, active_epoch;
NETIF_DECLARE_EXT_CALLBACK(netif_listener);

void network_changed(void)
{
    if (worker) rt_event_send(&wakeup, 1);
}

static void netif_changed(struct netif *netif, netif_nsc_reason_t reason,
                          const netif_ext_callback_args_t *args)
{
    (void)args;
    rt_mutex_take(&lock, RT_WAITING_FOREVER);
    for (unsigned source = NETWORK_PAN; source <= NETWORK_WIFI; ++source)
    {
        if (!interfaces[source][0] || netif->name[0] != interfaces[source][0] ||
            netif->name[1] != interfaces[source][1]) continue;
        ++epochs[source];
        if ((reason & LWIP_NSC_IPV4_SETTINGS_CHANGED) &&
            !ip4_addr_isany_val(*netif_ip4_addr(netif)))
        {
            /* This SDK processes DHCP DNS options before publishing the lease. */
            for (unsigned i = 0; i < DNS_MAX_SERVERS; ++i) resolvers[source][i] = *dns_getserver(i);
            dns_valid[source] = true;
        }
    }
    rt_mutex_release(&lock);
    /* Query later on tcpip_thread: DOWN/REMOVE callbacks precede the change. */
    network_changed();
}

static void sample(void *parameter)
{
    if (parameter) netif_add_ext_callback(&netif_listener, netif_changed);
    memset(&candidate, 0, sizeof(candidate));
    sampled_epoch = 0;
    struct netif *chosen = NULL;
    rt_mutex_take(&lock, RT_WAITING_FOREVER);
    network_source_t order[] = {NETWORK_WIFI, NETWORK_PAN};
    for (unsigned i = 0; i < 2; ++i)
    {
        network_source_t source = order[i];
        if (selected != NETWORK_AUTO && selected != source) continue;
        if (!interfaces[source][0] || (source == NETWORK_PAN && !btpan_is_network_ready())) continue;
        /* SDK device names already contain a digit. Match those two bytes,
         * independently of lwIP's separately allocated netif->num. */
        struct netif *netif;
        NETIF_FOREACH(netif)
            if (netif->name[0] == interfaces[source][0] && netif->name[1] == interfaces[source][1]) break;
        if (!netif || !netif_is_up(netif) || !netif_is_link_up(netif) ||
            ip4_addr_isany_val(*netif_ip4_addr(netif)) || ip4_addr_isany_val(*netif_ip4_gw(netif))) continue;
        chosen = netif;
        candidate.ready = true;
        candidate.source = source;
        candidate.address = ip4_addr_get_u32(netif_ip4_addr(netif));
        sampled_epoch = epochs[source];
        if (dns_valid[source])
            for (unsigned n = 0; n < DNS_MAX_SERVERS; ++n) dns_setserver(n, &resolvers[source][n]);
        break;
    }
    rt_mutex_release(&lock);
    if (netif_default != chosen) netif_set_default(chosen);
    rt_sem_release(&sampled);
}

static void network_worker(void *parameter)
{
    (void)parameter;
    bool first = true;
    for (;;)
    {
        if (tcpip_callback(sample, first ? &state : NULL) == ERR_OK)
        {
            rt_sem_take(&sampled, RT_WAITING_FOREVER);
            first = false;
            rt_mutex_take(&lock, RT_WAITING_FOREVER);
            if (state.ready != candidate.ready || state.source != candidate.source ||
                state.address != candidate.address || active_epoch != sampled_epoch)
            {
                candidate.generation = state.generation + 1;
                state = candidate;
                active_epoch = sampled_epoch;
                for (network_subscription_t *s = subscribers; s; s = s->next)
                    rt_event_send(s->event, s->bits);
            }
            rt_mutex_release(&lock);
        }
        else { rt_thread_mdelay(100); continue; }
        uint32_t events;
        rt_event_recv(&wakeup, 1, RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, RT_WAITING_FOREVER, &events);
    }
}

rt_err_t network_init(void)
{
    if (worker) return RT_EOK;
    rt_mutex_init(&lock, "net_sub", RT_IPC_FLAG_PRIO);
    rt_event_init(&wakeup, "net_w", RT_IPC_FLAG_FIFO);
    rt_sem_init(&sampled, "net_q", 0, RT_IPC_FLAG_FIFO);
    worker = rt_thread_create("network", network_worker, NULL, 2048, 22, 10);
    if (worker && rt_thread_startup(worker) == RT_EOK) return RT_EOK;
    if (worker) rt_thread_delete(worker);
    worker = NULL;
    rt_sem_detach(&sampled);
    rt_event_detach(&wakeup);
    rt_mutex_detach(&lock);
    return -RT_ENOMEM;
}

void network_snapshot(network_snapshot_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!worker) return;
    rt_mutex_take(&lock, RT_WAITING_FOREVER);
    *out = state;
    rt_mutex_release(&lock);
}
RTM_EXPORT(network_snapshot);
bool network_ready(void) { network_snapshot_t value; network_snapshot(&value); return value.ready; }
RTM_EXPORT(network_ready);

void network_select(network_source_t source)
{
    if (!worker || (unsigned)source > NETWORK_WIFI) return;
    rt_mutex_take(&lock, RT_WAITING_FOREVER);
    selected = source;
    rt_mutex_release(&lock);
    network_changed();
}

rt_err_t network_register_interface(network_source_t source, const char *name)
{
    if (!worker || source < NETWORK_PAN || source > NETWORK_WIFI || !name ||
        (*name && strlen(name) != 2))
        return -RT_EINVAL;
    rt_mutex_take(&lock, RT_WAITING_FOREVER);
    strcpy(interfaces[source], name);
    dns_valid[source] = false;
    ++epochs[source];
    rt_mutex_release(&lock);
    network_changed();
    return RT_EOK;
}

void network_set_dns(network_source_t source, uint32_t primary, uint32_t secondary)
{
    if (!worker || source < NETWORK_PAN || source > NETWORK_WIFI) return;
    rt_mutex_take(&lock, RT_WAITING_FOREVER);
    for (unsigned i = 0; i < DNS_MAX_SERVERS; ++i)
        ip_addr_set_ip4_u32(&resolvers[source][i], i == 0 ? primary : i == 1 ? secondary : 0);
    dns_valid[source] = true;
    ++epochs[source];
    rt_mutex_release(&lock);
    network_changed();
}

uint32_t network_subscribe(rt_event_t event, uint32_t bits)
{
    if (!worker || !event || !bits) return 0;
    network_subscription_t *s = rt_malloc(sizeof(*s));
    if (!s) return 0;
    rt_mutex_take(&lock, RT_WAITING_FOREVER);
    if (next_token == UINT32_MAX)
    {
        rt_mutex_release(&lock);
        rt_free(s);
        return 0;
    }
    uint32_t token = ++next_token;
    *s = (network_subscription_t){subscribers, event, bits, token};
    subscribers = s;
    rt_event_send(event, bits); /* Initial snapshot, including already-online. */
    rt_mutex_release(&lock);
    return token;
}
RTM_EXPORT(network_subscribe);

void network_unsubscribe(uint32_t token)
{
    if (!worker || !token) return;
    rt_mutex_take(&lock, RT_WAITING_FOREVER);
    network_subscription_t **link = &subscribers;
    while (*link && (*link)->token != token) link = &(*link)->next;
    network_subscription_t *removed = *link;
    if (removed) *link = removed->next;
    rt_mutex_release(&lock);
    rt_free(removed);
}
RTM_EXPORT(network_unsubscribe);
