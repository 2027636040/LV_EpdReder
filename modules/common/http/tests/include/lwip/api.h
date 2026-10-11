#ifndef HTTP_TEST_LWIP_API_H
#define HTTP_TEST_LWIP_API_H
#include <stdint.h>
#define ERR_OK 0
typedef struct { uint32_t address; } ip_addr_t;
#define ip_2_ip4(ip) (ip)
#define ip4_addr_get_u32(ip) ((ip)->address)
int netconn_gethostbyname(const char *host, ip_addr_t *ip);
#endif
