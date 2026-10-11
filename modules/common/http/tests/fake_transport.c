/* SPDX-License-Identifier: Apache-2.0 */
#include "fake_transport.h"
#include "epd_memory.h"
#include "network.h"
#include <tls_client.h>
#include <lwip/api.h>
#include <lwip/sockets.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

fake_transport_t fake;
typedef union { long double align; void *pointer; struct { unsigned magic; } data; } allocation_t;

void fake_assert_clean(void) { assert(fake.live == 0); assert(fake.sockets == fake.closes); }

void fake_reset(const void *wire, size_t size)
{
    fake_assert_clean();
    memset(&fake, 0, sizeof(fake));
    fake.wire = wire;
    fake.wire_size = size;
    fake.fragment = 17;
    fake.write_fragment = 13;
    fake.ready = true;
    fake.generation = 7;
    fake.eof = MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
    fake.authmode = MBEDTLS_SSL_VERIFY_REQUIRED;
}

void fake_reset_text(const char *wire) { fake_reset(wire, strlen(wire)); }

void *rt_malloc(size_t size)
{
    ++fake.allocations;
    if (size > fake.max_allocation) fake.max_allocation = size;
    if (fake.allocations == fake.fail_allocation || size > 16u * 1024u * 1024u) return NULL;
    allocation_t *allocation = malloc(sizeof(*allocation) + size);
    assert(allocation);
    allocation->data.magic = 0xaabbccdd;
    ++fake.live;
    return allocation + 1;
}

void rt_free(void *pointer)
{
    if (!pointer) return;
    allocation_t *allocation = (allocation_t *)pointer - 1;
    assert(allocation->data.magic == 0xaabbccdd && fake.live);
    allocation->data.magic = 0;
    --fake.live;
    free(allocation);
}

void *rt_calloc(size_t count, size_t size)
{
    assert(!size || count <= SIZE_MAX / size);
    void *p = rt_malloc(count * size);
    if (p) memset(p, 0, count * size);
    return p;
}

char *rt_strdup(const char *text)
{
    size_t size = strlen(text) + 1;
    char *p = rt_malloc(size);
    if (p) memcpy(p, text, size);
    return p;
}

void *epd_app_alloc(size_t size, epd_app_memory_t region)
{
    assert(region == EPD_APP_PSRAM);
    ++fake.psram_allocations;
    return rt_malloc(size);
}

void epd_app_free(void *pointer) { rt_free(pointer); }
rt_tick_t rt_tick_get(void) { return fake.ticks; }
void rt_thread_mdelay(int ms) { fake.ticks += (uint32_t)ms; }
bool weather_cancelled(void) { return fake.weather_cancel || fake.cancelled; }

bool fake_cancelled(void *user)
{
    assert(user == &fake);
    if (fake.cancel_at_snapshot && fake.snapshot_calls >= fake.cancel_at_snapshot) return true;
    if (fake.cancel_stage && fake.observed_response && fake.observed_response->stage &&
        !strcmp(fake.cancel_stage, fake.observed_response->stage)) return true;
    return fake.cancelled;
}

void network_snapshot(network_snapshot_t *out)
{
    ++fake.snapshot_calls;
    if (fake.generation_at_snapshot && fake.snapshot_calls == fake.generation_at_snapshot)
        ++fake.generation;
    memset(out, 0, sizeof(*out));
    out->ready = fake.ready;
    out->generation = fake.generation;
    out->address = 0x01020304;
}

bool network_ready(void) { return fake.ready; }

int netconn_gethostbyname(const char *host, ip_addr_t *ip)
{
    assert(host && *host);
    ++fake.dns_calls;
    fake.ticks += fake.dns_delay;
    if (fake.generation_in_dns) ++fake.generation;
    if (fake.cancel_in_dns) fake.cancelled = true;
    ip->address = 0x08080808;
    return fake.dns_error;
}

unsigned short htons(unsigned short value) { return value; }
int socket(int family, int type, int protocol)
{
    assert(family == AF_INET && type == SOCK_STREAM && protocol == IPPROTO_TCP);
    ++fake.sockets;
    return 5;
}
int ioctlsocket(int fd, long command, void *value)
{
    assert(fd == 5 && command == FIONBIO && *(int *)value == 1);
    return 0;
}
int bind(int fd, const struct sockaddr *address, socklen_t size)
{
    assert(fd == 5 && size == sizeof(struct sockaddr_in));
    fake.bound_address = ((const struct sockaddr_in *)address)->sin_addr.s_addr;
    return 0;
}
int connect(int fd, const struct sockaddr *address, socklen_t size)
{
    assert(fd == 5 && size == sizeof(struct sockaddr_in));
    assert(((const struct sockaddr_in *)address)->sin_port == 443);
    if (fake.connect_error) { errno = fake.connect_error; return -1; }
    if (fake.select_waits) { errno = EINPROGRESS; return -1; }
    return 0;
}
int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *errorfds, struct timeval *timeout)
{
    (void)readfds; (void)errorfds;
    assert(nfds == 6 && writefds->fd == 5);
    assert(timeout->tv_sec == 0 && timeout->tv_usec <= 100000);
    ++fake.select_calls;
    if (fake.select_calls <= fake.select_waits) { fake.ticks += 100; return 0; }
    return 1;
}
int getsockopt(int fd, int level, int name, void *value, socklen_t *size)
{
    assert(fd == 5 && level == SOL_SOCKET && name == SO_ERROR && *size == sizeof(int));
    *(int *)value = fake.socket_error;
    return 0;
}

int mbedtls_client_init(MbedTLSSession *session, void *entropy, size_t size)
{
    assert(size == 8 && !memcmp(entropy, "epd-http", size));
    session->server_fd.fd = -1;
    return fake.init_error;
}
int mbedtls_client_context(MbedTLSSession *session)
{
    assert(session->host);
    session->ssl.conf = &session->conf;
    session->conf.authmode = fake.authmode;
    return fake.context_error;
}
int mbedtls_client_close(MbedTLSSession *session)
{
    if (session->server_fd.fd >= 0) ++fake.closes;
    rt_free(session->host);
    rt_free(session);
    return 0;
}
void mbedtls_ssl_set_bio(mbedtls_ssl_context *ssl, void *ctx,
    int (*send)(void *, const unsigned char *, size_t),
    int (*recv)(void *, unsigned char *, size_t), void *timeout)
{
    (void)ssl; (void)ctx; (void)send; (void)recv; (void)timeout;
}
int mbedtls_net_send(void *ctx, const unsigned char *buffer, size_t size)
{ (void)ctx; (void)buffer; (void)size; assert(0); return -1; }
int mbedtls_net_recv(void *ctx, unsigned char *buffer, size_t size)
{ (void)ctx; (void)buffer; (void)size; assert(0); return -1; }
int mbedtls_ssl_handshake(mbedtls_ssl_context *ssl)
{
    assert(ssl->conf->authmode == MBEDTLS_SSL_VERIFY_REQUIRED);
    ++fake.handshake_calls;
    if (fake.handshake_calls <= fake.handshake_waits) return MBEDTLS_ERR_SSL_WANT_READ;
    return fake.handshake_error;
}
uint32_t mbedtls_ssl_get_verify_result(mbedtls_ssl_context *ssl) { (void)ssl; return fake.verify_flags; }
int mbedtls_ssl_read(mbedtls_ssl_context *ssl, unsigned char *buffer, size_t size)
{
    (void)ssl;
    ++fake.read_calls;
    if (fake.read_calls <= fake.read_waits) return MBEDTLS_ERR_SSL_WANT_READ;
    if (fake.read_error) return fake.read_error;
    if (fake.received == fake.wire_size) return fake.eof;
    if (size > fake.fragment) size = fake.fragment;
    if (size > fake.wire_size - fake.received) size = fake.wire_size - fake.received;
    memcpy(buffer, fake.wire + fake.received, size);
    fake.received += size;
    return (int)size;
}
int mbedtls_ssl_write(mbedtls_ssl_context *ssl, const unsigned char *buffer, size_t size)
{
    (void)ssl;
    ++fake.write_calls;
    if (fake.write_calls <= fake.write_waits) return MBEDTLS_ERR_SSL_WANT_WRITE;
    if (fake.write_error) return fake.write_error;
    if (size > fake.write_fragment) size = fake.write_fragment;
    assert(fake.sent_size + size < sizeof(fake.sent));
    memcpy(fake.sent + fake.sent_size, buffer, size);
    fake.sent_size += size;
    fake.sent[fake.sent_size] = 0;
    return (int)size;
}
