#ifndef EPD_NETWORK_H
#define EPD_NETWORK_H
#include <rtthread.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum { NETWORK_AUTO, NETWORK_PAN, NETWORK_WIFI } network_source_t;
typedef struct
{
    bool ready;
    network_source_t source;
    uint32_t generation;
    uint32_t address; /* IPv4, network byte order. */
} network_snapshot_t;

rt_err_t network_init(void);
void network_snapshot(network_snapshot_t *out);
bool network_ready(void);
void network_changed(void);
void network_select(network_source_t source);
/* Adapter registration: SDK two-character lwIP device name, e.g. b0 or w0. */
rt_err_t network_register_interface(network_source_t source, const char *name);
/* Static addressing or DHCP renewals may supply per-interface DNS explicitly.
 * Values are IPv4 network byte order; zero clears that DNS slot. */
void network_set_dns(network_source_t source, uint32_t primary, uint32_t secondary);
/* No application callback executes on the network thread. The event must remain
 * alive until unsubscribe returns; that return drains in-flight event sends. */
uint32_t network_subscribe(rt_event_t event, uint32_t bits);
void network_unsubscribe(uint32_t subscription);
#endif
