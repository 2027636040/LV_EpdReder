/* SPDX-License-Identifier: Apache-2.0 */
#include "weather_http.h"
#include "bt_pan.h"
#include <tls_client.h>
#include <lwip/api.h>
#include <lwip/tcpip.h>
#include <lwip/netif.h>
#include <lwip/sockets.h>
#include <miniz.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>

#define DBG_TAG "weather.http"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define BODY_MAX 32768u
#define BODY_INITIAL 1024u
#define HTTP_TIMEOUT_MS 30000

typedef struct
{
    MbedTLSSession *tls;
    rt_tick_t start;
    const char *stage;
    int detail;
    uint32_t verify_flags;
} http_reader_t;

static void allocation_failed(const char *stage, size_t bytes)
{
    rt_uint32_t total, used, peak;
    rt_memory_info(&total, &used, &peak);
    LOG_E("stage=%s alloc=%u failed; heap_free=%u peak=%u", stage,
          (unsigned)bytes, (unsigned)(total - used), (unsigned)peak);
}

static bool body_reserve(char **body, size_t *capacity, size_t needed, bool exact)
{
    if (needed > BODY_MAX) return false;
    if (*body && needed <= *capacity) return true;
    size_t next = exact ? needed : (*capacity ? *capacity : BODY_INITIAL);
    while (next < needed) next = next > BODY_MAX / 2 ? BODY_MAX : next * 2;
    char *resized = rt_realloc(*body, next + 1);
    if (!resized)
    {
        allocation_failed("body-alloc", next + 1);
        return false;
    }
    *body = resized;
    *capacity = next;
    return true;
}

static struct rt_semaphore network_checked;
static bool network_check_initialized, network_ready;

static void network_check(void *context)
{
    struct netif *netif;
    (void)context;
    network_ready = false;
    NETIF_FOREACH(netif)
    {
        if (netif->name[0] == 'b' && netif_is_up(netif) && netif_is_link_up(netif) &&
            !ip4_addr_isany_val(*netif_ip4_addr(netif)) && !ip4_addr_isany_val(*netif_ip4_gw(netif)))
        {
            netif_set_default(netif);
            network_ready = true;
            break;
        }
    }
    rt_sem_release(&network_checked);
}

/* Used only by the weather worker. Netif traversal runs on lwIP's own thread. */
bool weather_network_ready(void)
{
    if (!btpan_is_network_ready()) return false;
    if (!network_check_initialized)
    {
        rt_sem_init(&network_checked, "wx_net", 0, RT_IPC_FLAG_FIFO);
        network_check_initialized = true;
    }
    if (tcpip_callback(network_check, RT_NULL) != ERR_OK) return false;
    rt_sem_take(&network_checked, RT_WAITING_FOREVER);
    return network_ready;
}

static bool expired(http_reader_t *reader)
{
    if (!btpan_is_network_ready())
    {
        reader->detail = -RT_ERROR;
        return true;
    }
    if ((rt_tick_t)(rt_tick_get() - reader->start) >= rt_tick_from_millisecond(HTTP_TIMEOUT_MS))
    {
        reader->detail = -RT_ETIMEOUT;
        return true;
    }
    return false;
}

static int tls_read(http_reader_t *reader, unsigned char *buffer, size_t size)
{
    while (!expired(reader))
    {
        int ret = mbedtls_ssl_read(&reader->tls->ssl, buffer, size);
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            reader->detail = ret;
            return ret;
        }
        rt_thread_mdelay(10);
    }
    return -1;
}

static bool tls_read_exact(http_reader_t *reader, unsigned char *buffer, size_t size, size_t *received)
{
    while (size)
    {
        int count = tls_read(reader, buffer, size);
        if (count <= 0) return false;
        buffer += count;
        size -= count;
        *received += count;
    }
    return true;
}

static int tls_line(http_reader_t *reader, char *line, size_t capacity)
{
    size_t used = 0;
    while (used + 1 < capacity)
    {
        unsigned char ch;
        if (tls_read(reader, &ch, 1) != 1) return -1;
        if (ch == '\n')
        {
            if (!used || line[used - 1] != '\r')
            {
                reader->detail = -RT_EINVAL;
                return -1;
            }
            line[--used] = '\0';
            return (int)used;
        }
        line[used++] = ch;
    }
    reader->detail = -RT_EFULL;
    return -1;
}

static bool tls_connect(http_reader_t *reader, const char *host)
{
    ip_addr_t ip;
    struct sockaddr_in address;
    int ret, nonblocking = 1, socket_error = 0;
    socklen_t error_size = sizeof(socket_error);
    reader->stage = "dns";
    reader->detail = netconn_gethostbyname(host, &ip);
    if (reader->detail != ERR_OK) return false;
    reader->start = rt_tick_get();
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(443);
    address.sin_addr.s_addr = ip4_addr_get_u32(ip_2_ip4(&ip));
    reader->stage = "tcp-connect";
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) { reader->detail = errno; return false; }
    reader->tls->server_fd.fd = fd;
    if (ioctlsocket(fd, FIONBIO, &nonblocking) != 0) { reader->detail = errno; return false; }
    ret = connect(fd, (struct sockaddr *)&address, sizeof(address));
    if (ret < 0 && errno != EINPROGRESS && errno != EWOULDBLOCK)
    {
        reader->detail = errno;
        return false;
    }
    while (ret < 0 && !expired(reader))
    {
        fd_set writefds;
        struct timeval timeout = {0, 200000};
        FD_ZERO(&writefds);
        FD_SET(fd, &writefds);
        ret = select(fd + 1, RT_NULL, &writefds, RT_NULL, &timeout);
        if (ret < 0) { reader->detail = errno; return false; }
        if (ret > 0)
        {
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_size) != 0)
            { reader->detail = errno; return false; }
            if (socket_error) { reader->detail = socket_error; return false; }
            break;
        }
        ret = -1;
    }
    if (expired(reader)) return false;
    mbedtls_ssl_set_bio(&reader->tls->ssl, &reader->tls->server_fd,
                        mbedtls_net_send, mbedtls_net_recv, RT_NULL);
    reader->stage = "tls-handshake";
    while (!expired(reader))
    {
        ret = mbedtls_ssl_handshake(&reader->tls->ssl);
        reader->verify_flags = mbedtls_ssl_get_verify_result(&reader->tls->ssl);
        if (!ret)
        {
            reader->stage = "tls-verify";
            if (reader->verify_flags) return false;
            reader->detail = 0;
            LOG_I("TLS certificate verified");
            return true;
        }
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
        { reader->detail = ret; return false; }
        rt_thread_mdelay(10);
    }
    return false;
}

static char *gunzip(const unsigned char *src, size_t size)
{
    size_t offset = 10;
    if (size < 18 || src[0] != 0x1f || src[1] != 0x8b || src[2] != 8 || (src[3] & 0xe0))
    {
        LOG_E("stage=gzip-header invalid; bytes=%u", (unsigned)size);
        return RT_NULL;
    }
    if (src[3] & 4) offset = 12 + src[10] + ((size_t)src[11] << 8);
    if (src[3] & 8) { while (offset < size - 8 && src[offset]) ++offset; ++offset; }
    if (src[3] & 16) { while (offset < size - 8 && src[offset]) ++offset; ++offset; }
    if (src[3] & 2) offset += 2;
    if (offset >= size - 8)
    {
        LOG_E("stage=gzip-header invalid offset=%u bytes=%u", (unsigned)offset, (unsigned)size);
        return RT_NULL;
    }
    if (src[3] & 2)
    {
        uint16_t header_crc = src[offset - 2] | ((uint16_t)src[offset - 1] << 8);
        if ((mz_crc32(0, src, offset - 2) & 0xffffu) != header_crc)
        {
            LOG_E("stage=gzip-header CRC mismatch");
            return RT_NULL;
        }
    }
    uint32_t expected_crc = 0, expected_size = 0;
    for (unsigned i = 0; i < 4; ++i)
    {
        expected_crc |= (uint32_t)src[size - 8 + i] << (8 * i);
        expected_size |= (uint32_t)src[size - 4 + i] << (8 * i);
    }
    if (!expected_size || expected_size > BODY_MAX)
    {
        LOG_E("stage=gzip-size invalid decoded_size=%u", (unsigned)expected_size);
        return RT_NULL;
    }
    unsigned char *output = rt_malloc(expected_size + 1);
    if (!output)
    {
        allocation_failed("gzip-output-alloc", expected_size + 1);
        return RT_NULL;
    }
    tinfl_decompressor *decoder = tinfl_decompressor_alloc();
    bool ok = false;
    if (decoder)
    {
        size_t input_size = size - 8 - offset, output_size = expected_size;
        tinfl_init(decoder);
        tinfl_status result = tinfl_decompress(decoder, src + offset, &input_size,
            output, output, &output_size, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        uint32_t actual_crc = mz_crc32(0, output, output_size);
        if (result != TINFL_STATUS_DONE)
            LOG_E("stage=gzip-inflate ret=%d in=%u/%u out=%u/%u", result,
                  (unsigned)input_size, (unsigned)(size - 8 - offset),
                  (unsigned)output_size, (unsigned)expected_size);
        else if (output_size != expected_size || input_size != size - 8 - offset)
            LOG_E("stage=gzip-length in=%u/%u out=%u/%u", (unsigned)input_size,
                  (unsigned)(size - 8 - offset), (unsigned)output_size, (unsigned)expected_size);
        else if (actual_crc != expected_crc)
            LOG_E("stage=gzip-crc actual=%08x expected=%08x", (unsigned)actual_crc, (unsigned)expected_crc);
        else ok = true;
        if (ok) output[output_size] = '\0';
    }
    else allocation_failed("gzip-decoder-alloc", sizeof(tinfl_decompressor));
    if (decoder) tinfl_decompressor_free(decoder);
    if (!ok) { rt_free(output); output = RT_NULL; }
    else LOG_I("gzip decoded: wire=%u json=%u", (unsigned)size, (unsigned)expected_size);
    return (char *)output;
}

char *weather_http_get(const weather_config_t *config, const char *path, int *status)
{
    http_reader_t reader = {0};
    char line[768];
    char *body = RT_NULL;
    size_t used = 0, capacity = 0, headers_size = 0;
    long content_length = -1;
    bool chunked = false, gzip = false, complete = false;
    int ret;
    *status = 0;
    reader.stage = "tls-session-alloc";
    reader.start = rt_tick_get();
    reader.tls = rt_calloc(1, sizeof(*reader.tls));
    if (!reader.tls)
    {
        allocation_failed(reader.stage, sizeof(*reader.tls));
        return RT_NULL;
    }
    reader.stage = "tls-init";
    reader.detail = mbedtls_client_init(reader.tls, "epd-weather", 11);
    if (reader.detail != 0) goto finish;
    reader.stage = "tls-host-alloc";
    reader.tls->host = rt_strdup(config->api_host);
    if (!reader.tls->host)
    {
        allocation_failed(reader.stage, strlen(config->api_host) + 1);
        goto finish;
    }
    reader.stage = "tls-context";
    reader.detail = mbedtls_client_context(reader.tls);
    if (reader.detail != 0 || !tls_connect(&reader, config->api_host)) goto finish;
    reader.stage = "request-build";
    ret = rt_snprintf(line, sizeof(line),
        "GET %s HTTP/1.1\r\nHost: %s\r\nX-QW-Api-Key: %s\r\n"
        "Accept: application/json\r\nAccept-Encoding: gzip\r\nConnection: close\r\n\r\n",
        path, config->api_host, config->api_key);
    if (ret <= 0 || ret >= sizeof(line)) goto finish;
    reader.stage = "request-write";
    size_t sent = 0;
    while (sent < (size_t)ret && !expired(&reader))
    {
        int n = mbedtls_ssl_write(&reader.tls->ssl, (unsigned char *)line + sent, ret - sent);
        if (n > 0) sent += n;
        else if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) rt_thread_mdelay(10);
        else { reader.detail = n; goto finish; }
    }
    if (sent != (size_t)ret) goto finish;
    memset(line, 0, sizeof(line));
    reader.stage = "status-read";
    if (tls_line(&reader, line, sizeof(line)) < 12 || strncmp(line, "HTTP/1.", 7)) goto finish;
    *status = atoi(line + 9);
    reader.stage = "http-status";
    /* Redirects are not followed; credentials never leave the configured host. */
    if (*status != 200) goto finish;
    for (;;)
    {
        reader.stage = "headers-read";
        int n = tls_line(&reader, line, sizeof(line));
        if (n < 0) goto finish;
        if ((headers_size += n + 2) > 4096)
        { reader.detail = -RT_EFULL; goto finish; }
        if (!n) break;
        for (int i = 0; i < n; ++i) line[i] = (char)tolower((unsigned char)line[i]);
        while (n && (line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = '\0';
        if (!strncmp(line, "content-length:", 15))
        {
            reader.stage = "headers-content-length";
            char *end, *value = line + 15;
            while (*value == ' ' || *value == '\t') ++value;
            if (!isdigit((unsigned char)*value)) goto finish;
            errno = 0;
            unsigned long length = strtoul(value, &end, 10);
            if (errno == ERANGE || *end || length > BODY_MAX ||
                (content_length >= 0 && (unsigned long)content_length != length)) goto finish;
            content_length = (long)length;
        }
        else if (!strncmp(line, "transfer-encoding:", 18))
        {
            reader.stage = "headers-transfer-encoding";
            const char *value = line + 18;
            while (*value == ' ' || *value == '\t') ++value;
            if (strcmp(value, "chunked")) goto finish;
            chunked = true;
        }
        else if (!strncmp(line, "content-encoding:", 17))
        {
            reader.stage = "headers-content-encoding";
            const char *value = line + 17;
            while (*value == ' ' || *value == '\t') ++value;
            if (!strcmp(value, "gzip")) gzip = true;
            else if (strcmp(value, "identity")) goto finish;
        }
    }
    LOG_I("HTTP %d length=%ld chunked=%d gzip=%d", *status, content_length, chunked, gzip);
    if (chunked)
    {
        reader.stage = "chunk-size";
        while (!expired(&reader))
        {
            char *end;
            reader.stage = "chunk-size";
            if (tls_line(&reader, line, sizeof(line)) <= 0 || !isxdigit((unsigned char)line[0])) goto finish;
            errno = 0;
            unsigned long chunk = strtoul(line, &end, 16);
            if (errno == ERANGE || (*end && *end != ';') || chunk > BODY_MAX - used) goto finish;
            if (!chunk) { complete = true; break; }
            reader.stage = "body-alloc";
            if (!body_reserve(&body, &capacity, used + chunk, false)) goto finish;
            reader.stage = "chunk-read";
            if (!tls_read_exact(&reader, (unsigned char *)body + used, chunk, &used)) goto finish;
            reader.stage = "chunk-terminator";
            if (tls_line(&reader, line, sizeof(line)) != 0) goto finish;
        }
    }
    else if (content_length >= 0)
    {
        reader.stage = "body-alloc";
        if (!body_reserve(&body, &capacity, (size_t)content_length, true)) goto finish;
        reader.stage = "body-read";
        complete = tls_read_exact(&reader, (unsigned char *)body, (size_t)content_length, &used);
    }
    else
    {
        reader.stage = "body-read";
        while (!expired(&reader))
        {
            if (used == BODY_MAX)
            {
                unsigned char extra;
                ret = tls_read(&reader, &extra, 1);
                complete = !ret || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
                if (!complete && ret > 0) reader.stage = "body-limit";
                break;
            }
            reader.stage = "body-alloc";
            if (!body_reserve(&body, &capacity, used + 1, false)) goto finish;
            reader.stage = "body-read";
            ret = tls_read(&reader, (unsigned char *)body + used, capacity - used);
            if (!ret || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) { complete = true; break; }
            if (ret < 0) break;
            used += ret;
        }
    }
finish:
    memset(line, 0, sizeof(line));
    /* Release TLS buffers before allocating the gzip decoder and JSON tree. */
    mbedtls_client_close(reader.tls);
    if (!complete || !used)
    {
        LOG_E("stage=%s HTTP=%d detail=%d verify=%08x rx=%u expected=%ld pan=%d",
              reader.stage, *status, reader.detail, (unsigned)reader.verify_flags,
              (unsigned)used, content_length, btpan_is_network_ready());
        rt_free(body);
        return RT_NULL;
    }
    LOG_I("body received: bytes=%u capacity=%u", (unsigned)used, (unsigned)capacity);
    body[used] = '\0';
    if (gzip || (used >= 2 && (unsigned char)body[0] == 0x1f && (unsigned char)body[1] == 0x8b))
    {
        char *plain = gunzip((unsigned char *)body, used);
        rt_free(body);
        return plain;
    }
    return body;
}
