/* SPDX-License-Identifier: Apache-2.0 */
#include "epd_http.h"
#include "epd_memory.h"
#include "network.h"
#include <tls_client.h>
#include <lwip/api.h>
#include <lwip/sockets.h>
#include <miniz.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <errno.h>

#define LINE_SIZE 4096u
#define HEADERS_LIMIT 32768u
#define BODY_INITIAL 1024u
#define IO_SLICE 1024u

typedef struct
{
    const epd_http_request_t *request;
    epd_http_response_t *response;
    MbedTLSSession *tls;
    network_snapshot_t route;
    rt_tick_t start;
    uint64_t timeout_ticks;
    size_t limit;
} http_reader_t;

static bool fail(http_reader_t *reader, const char *stage, int detail)
{
    reader->response->stage = stage;
    reader->response->detail = detail;
    return false;
}

static bool expired(http_reader_t *reader)
{
    network_snapshot_t route;
    if (reader->request->cancelled && reader->request->cancelled(reader->request->user))
        return !fail(reader, "cancelled", -RT_ERROR);
    network_snapshot(&route);
    if (!route.ready || route.generation != reader->route.generation)
        return !fail(reader, "network-changed", -RT_ERROR);
    if ((uint64_t)(rt_tick_t)(rt_tick_get() - reader->start) >= reader->timeout_ticks)
        return !fail(reader, "timeout", -RT_ETIMEOUT);
    return false;
}

static unsigned char lower(unsigned char ch)
{
    return ch >= 'A' && ch <= 'Z' ? (unsigned char)(ch + 'a' - 'A') : ch;
}

static bool equal(const char *a, const char *b)
{
    while (*a && lower((unsigned char)*a) == lower((unsigned char)*b)) { ++a; ++b; }
    return *a == *b;
}

static bool alpha(unsigned char ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

static bool digit(unsigned char ch) { return ch >= '0' && ch <= '9'; }

static bool token(unsigned char ch)
{
    return alpha(ch) || digit(ch) || (ch && strchr("!#$%&'*+-.^_`|~", ch));
}

/* Validate the authority before DNS or writing credentials to the connection. */
static bool valid_host(const char *host)
{
    size_t length = 0, label = 0;
    bool numeric = true;
    if (!host || !*host) return false;
    for (const unsigned char *p = (const unsigned char *)host; *p; ++p)
    {
        if (++length > 253) return false;
        if (*p == '.')
        {
            if (!label || p[-1] == '-') return false;
            label = 0;
        }
        else
        {
            if (!(alpha(*p) || digit(*p) || *p == '-') || (!label && *p == '-')) return false;
            if (++label > 63) return false;
            if (!digit(*p)) numeric = false;
        }
    }
    if (host[length - 1] == '-') return false;
    if (numeric)
    {
        unsigned parts = 0;
        const char *p = host;
        do
        {
            unsigned value = 0, digits = 0;
            while (digit((unsigned char)*p))
            {
                if (++digits > 3) return false;
                value = value * 10 + (unsigned)(*p++ - '0');
            }
            if (!digits || value > 255 || ++parts > 4) return false;
            if (!*p) break;
            if (*p++ != '.' || !*p) return false;
        } while (*p);
        return parts == 4;
    }
    return true;
}

static bool request_valid(http_reader_t *reader)
{
    const epd_http_request_t *request = reader->request;
    if (!valid_host(request->host) || !request->path || request->path[0] != '/' ||
        (request->method && strcmp(request->method, "GET") && strcmp(request->method, "POST")) ||
        (request->body_size && !request->body) || (request->header_count && !request->headers))
        return fail(reader, "request-invalid", -RT_EINVAL);
    for (const unsigned char *p = (const unsigned char *)request->path; *p; ++p)
        if (*p <= 32 || *p >= 127 || *p == '#') return fail(reader, "request-path", -RT_EINVAL);
    for (size_t i = 0; i < request->header_count; ++i)
    {
        if (expired(reader)) return false;
        const epd_http_header_t *header = &request->headers[i];
        if (!header->name || !*header->name || !header->value)
            return fail(reader, "request-header", -RT_EINVAL);
        for (const unsigned char *p = (const unsigned char *)header->name; *p; ++p)
            if (!token(*p)) return fail(reader, "request-header", -RT_EINVAL);
        for (const unsigned char *p = (const unsigned char *)header->value; *p; ++p)
            if ((*p < 32 && *p != '\t') || *p == 127)
                return fail(reader, "request-header", -RT_EINVAL);
        if (equal(header->name, "Host") || equal(header->name, "Connection") ||
            equal(header->name, "Content-Length") || equal(header->name, "Transfer-Encoding") ||
            equal(header->name, "Expect"))
            return fail(reader, "request-header-reserved", -RT_EINVAL);
    }
    return !expired(reader);
}

static bool body_reserve(http_reader_t *reader, char **body, size_t used,
                         size_t *capacity, size_t needed, bool exact)
{
    if (expired(reader)) return false;
    if (needed > reader->limit) return fail(reader, "body-limit", -RT_EFULL);
    if (*body && needed <= *capacity) return true;
    size_t next = exact ? needed : (*capacity ? *capacity : BODY_INITIAL);
    if (next > reader->limit) next = reader->limit;
    while (next < needed) next = next > reader->limit / 2 ? reader->limit : next * 2;
    char *resized = epd_app_alloc(next + 1, EPD_APP_PSRAM);
    if (!resized) return fail(reader, "body-alloc", -RT_ENOMEM);
    for (size_t offset = 0; offset < used;)
    {
        size_t count = used - offset > IO_SLICE ? IO_SLICE : used - offset;
        if (expired(reader)) { epd_app_free(resized); return false; }
        memcpy(resized + offset, *body + offset, count);
        offset += count;
    }
    epd_app_free(*body);
    *body = resized;
    *capacity = next;
    return true;
}

static int tls_read(http_reader_t *reader, unsigned char *buffer, size_t size)
{
    if (size > IO_SLICE) size = IO_SLICE;
    while (!expired(reader))
    {
        int ret = mbedtls_ssl_read(&reader->tls->ssl, buffer, size);
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            if (expired(reader)) return -1;
            reader->response->detail = ret;
            return ret;
        }
        rt_thread_mdelay(10);
    }
    return -1;
}

static bool tls_write(http_reader_t *reader, const void *buffer, size_t size)
{
    const unsigned char *bytes = buffer;
    while (size)
    {
        if (expired(reader)) return false;
        size_t slice = size > IO_SLICE ? IO_SLICE : size;
        int ret = mbedtls_ssl_write(&reader->tls->ssl, bytes, slice);
        if (ret > 0 && (size_t)ret <= slice) { bytes += ret; size -= (size_t)ret; }
        else if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
            rt_thread_mdelay(10);
        else return fail(reader, "request-write", ret ? ret : -RT_ERROR);
    }
    return !expired(reader);
}

static bool read_exact(http_reader_t *reader, char *body, size_t size, size_t *used)
{
    while (size)
    {
        int count = tls_read(reader, (unsigned char *)body + *used, size);
        if (count <= 0) return false;
        if ((size_t)count > size) return fail(reader, "body-read", -RT_EINVAL);
        *used += (size_t)count;
        size -= (size_t)count;
    }
    return true;
}

static int read_line(http_reader_t *reader, char *line)
{
    size_t used = 0;
    while (used < LINE_SIZE)
    {
        unsigned char ch;
        if (tls_read(reader, &ch, 1) != 1) return -1;
        if (ch == '\n')
        {
            if (!used || line[used - 1] != '\r') break;
            line[--used] = '\0';
            return (int)used;
        }
        if (!ch || (ch < 32 && ch != '\r' && ch != '\t') || ch == 127 ||
            (used && line[used - 1] == '\r')) break;
        line[used++] = (char)ch;
    }
    reader->response->detail = -RT_EINVAL;
    return -1;
}

static bool tls_connect(http_reader_t *reader)
{
    ip_addr_t ip;
    struct sockaddr_in address = {0}, local = {0};
    int ret, nonblocking = 1, socket_error = 0;
    socklen_t error_size = sizeof(socket_error);
    reader->response->stage = "dns";
    if (expired(reader)) return false;
    ret = netconn_gethostbyname(reader->request->host, &ip);
    if (expired(reader)) return false;
    if (ret != ERR_OK) return fail(reader, "dns", ret);
    address.sin_family = AF_INET;
    address.sin_port = htons(443);
    address.sin_addr.s_addr = ip4_addr_get_u32(ip_2_ip4(&ip));
    reader->response->stage = "tcp-connect";
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return fail(reader, "tcp-connect", errno);
    reader->tls->server_fd.fd = fd;
    if (ioctlsocket(fd, FIONBIO, &nonblocking) != 0) return fail(reader, "tcp-nonblock", errno);
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = reader->route.address;
    if (expired(reader)) return false;
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0) return fail(reader, "tcp-bind", errno);
    ret = connect(fd, (struct sockaddr *)&address, sizeof(address));
    if (ret < 0 && errno != EINPROGRESS && errno != EWOULDBLOCK)
        return fail(reader, "tcp-connect", errno);
    while (ret < 0)
    {
        fd_set writefds;
        struct timeval timeout = {0, 100000};
        if (expired(reader)) return false;
        FD_ZERO(&writefds);
        FD_SET(fd, &writefds);
        ret = select(fd + 1, RT_NULL, &writefds, RT_NULL, &timeout);
        if (expired(reader)) return false;
        if (ret < 0) return fail(reader, "tcp-select", errno);
        if (ret > 0)
        {
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_size) != 0)
                return fail(reader, "tcp-connect", errno);
            if (socket_error) return fail(reader, "tcp-connect", socket_error);
            break;
        }
        ret = -1;
    }
    if (expired(reader)) return false;
    mbedtls_ssl_set_bio(&reader->tls->ssl, &reader->tls->server_fd,
                        mbedtls_net_send, mbedtls_net_recv, RT_NULL);
    reader->response->stage = "tls-handshake";
    while (!expired(reader))
    {
        ret = mbedtls_ssl_handshake(&reader->tls->ssl);
        reader->response->verify_flags = mbedtls_ssl_get_verify_result(&reader->tls->ssl);
        if (expired(reader)) return false;
        if (!ret)
        {
            if (reader->response->verify_flags) return fail(reader, "tls-verify", -RT_ERROR);
            return true;
        }
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
            return fail(reader, "tls-handshake", ret);
        rt_thread_mdelay(10);
    }
    return false;
}

static bool append_text(http_reader_t *reader, char *buffer, size_t *used, const char *text)
{
    size_t size = strlen(text);
    if (size > SIZE_MAX - 1 - *used) return fail(reader, "request-size", -RT_EFULL);
    for (size_t offset = 0; offset < size;)
    {
        if (expired(reader)) return false;
        size_t count = size - offset > IO_SLICE ? IO_SLICE : size - offset;
        if (buffer) memcpy(buffer + *used + offset, text + offset, count);
        offset += count;
    }
    *used += size;
    return true;
}

static bool request_headers(http_reader_t *reader, char *buffer, size_t *used)
{
    const epd_http_request_t *request = reader->request;
    const char *method = request->method ? request->method : "GET";
    char length[3 * sizeof(size_t) + 4];
    if (!append_text(reader, buffer, used, method) || !append_text(reader, buffer, used, " ") ||
        !append_text(reader, buffer, used, request->path) || !append_text(reader, buffer, used, " HTTP/1.1\r\nHost: ") ||
        !append_text(reader, buffer, used, request->host) || !append_text(reader, buffer, used, "\r\nConnection: close\r\n")) return false;
    for (size_t i = 0; i < request->header_count; ++i)
        if (!append_text(reader, buffer, used, request->headers[i].name) || !append_text(reader, buffer, used, ": ") ||
            !append_text(reader, buffer, used, request->headers[i].value) || !append_text(reader, buffer, used, "\r\n")) return false;
    if (request->body_size || !strcmp(method, "POST"))
    {
        snprintf(length, sizeof(length), "%zu", request->body_size);
        if (!append_text(reader, buffer, used, "Content-Length: ") || !append_text(reader, buffer, used, length) ||
            !append_text(reader, buffer, used, "\r\n")) return false;
    }
    return append_text(reader, buffer, used, "\r\n");
}

static bool send_request(http_reader_t *reader)
{
    size_t size = 0, used = 0;
    reader->response->stage = "request-build";
    if (!request_headers(reader, NULL, &size)) return false;
    char *headers = epd_app_alloc(size + 1, EPD_APP_PSRAM);
    if (!headers) return fail(reader, "request-alloc", -RT_ENOMEM);
    bool ok = request_headers(reader, headers, &used);
    if (ok)
    {
        headers[used] = '\0';
        reader->response->stage = "request-write";
        ok = tls_write(reader, headers, used);
    }
    epd_app_free(headers);
    return ok && tls_write(reader, reader->request->body, reader->request->body_size);
}

static bool number(const char *value, unsigned base, size_t *result, const char **end)
{
    size_t total = 0;
    const char *p = value;
    for (;; ++p)
    {
        unsigned ch = lower((unsigned char)*p);
        unsigned n = digit((unsigned char)ch) ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : base;
        if (n >= base) break;
        if (total > (SIZE_MAX - n) / base) return false;
        total = total * base + n;
    }
    if (p == value) return false;
    *end = p;
    *result = total;
    return true;
}

static bool split_header(char *line, char **value)
{
    char *p = line;
    while (token((unsigned char)*p)) ++p;
    if (p == line || *p != ':') return false;
    *p++ = '\0';
    while (*p == ' ' || *p == '\t') ++p;
    *value = p;
    p += strlen(p);
    while (p > *value && (p[-1] == ' ' || p[-1] == '\t')) --p;
    *p = '\0';
    return true;
}

static bool header_callback(http_reader_t *reader, const char *name, const char *value)
{
    if (expired(reader)) return false;
    if (reader->request->on_header && !reader->request->on_header(reader->request->user, name, value))
        return fail(reader, "header-cancelled", -RT_ERROR);
    return !expired(reader);
}

static int header_line(http_reader_t *reader, char *line, size_t *total)
{
    int n = read_line(reader, line);
    if (n < 0) return -1;
    if ((size_t)n + 2 > HEADERS_LIMIT - *total)
    {
        fail(reader, "headers-limit", -RT_EFULL);
        return -1;
    }
    *total += (size_t)n + 2;
    return n;
}

static bool receive_body(http_reader_t *reader, char *line, char **body, size_t *used, bool *gzip)
{
    size_t total = 0, capacity = 0, length = 0;
    bool has_length = false, chunked = false, has_encoding = false;
    int status = 0;
    for (unsigned interim = 0;; ++interim)
    {
        reader->response->stage = "status-read";
        int n = header_line(reader, line, &total);
        if (n < 0) return false;
        if (n < 12 || (strncmp(line, "HTTP/1.0 ", 9) && strncmp(line, "HTTP/1.1 ", 9)) ||
            line[9] < '1' || line[9] > '5' || !digit((unsigned char)line[10]) ||
            !digit((unsigned char)line[11]) || (n > 12 && line[12] != ' '))
            return fail(reader, "status-read", -RT_EINVAL);
        status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + line[11] - '0';
        reader->response->status = status;
        if (status == 101 || interim >= 8) return fail(reader, "http-status", -RT_EINVAL);
        has_length = chunked = has_encoding = *gzip = false;
        for (;;)
        {
            char *value;
            reader->response->stage = "headers-read";
            n = header_line(reader, line, &total);
            if (n < 0) return false;
            if (!n) break;
            if (!split_header(line, &value)) return fail(reader, "headers-read", -RT_EINVAL);
            if (equal(line, "content-length"))
            {
                size_t parsed;
                const char *end;
                if (!number(value, 10, &parsed, &end) || *end || (has_length && length != parsed))
                    return fail(reader, "headers-content-length", -RT_EINVAL);
                length = parsed;
                has_length = true;
            }
            else if (equal(line, "transfer-encoding"))
            {
                if (chunked || !equal(value, "chunked"))
                    return fail(reader, "headers-transfer-encoding", -RT_EINVAL);
                chunked = true;
            }
            else if (equal(line, "content-encoding"))
            {
                if (has_encoding || (!equal(value, "gzip") && !equal(value, "identity")))
                    return fail(reader, "headers-content-encoding", -RT_EINVAL);
                *gzip = equal(value, "gzip");
                has_encoding = true;
            }
            if (status >= 200 && !header_callback(reader, line, value)) return false;
        }
        if ((chunked && has_length) || ((status < 200 || status == 204) && (chunked || has_length)))
            return fail(reader, "headers-framing", -RT_EINVAL);
        if (status >= 200) break;
    }
    if (status == 204 || status == 304)
    {
        *gzip = false;
        return body_reserve(reader, body, 0, &capacity, 0, true);
    }
    if (chunked)
    {
        for (;;)
        {
            size_t chunk;
            const char *end;
            reader->response->stage = "chunk-size";
            int n = read_line(reader, line);
            if (n < 0) return false;
            if (!number(line, 16, &chunk, &end) || (*end && *end != ';'))
                return fail(reader, "chunk-size", -RT_EINVAL);
            if (!chunk) break;
            if (chunk > reader->limit - *used) return fail(reader, "body-limit", -RT_EFULL);
            if (!body_reserve(reader, body, *used, &capacity, *used + chunk, false)) return false;
            reader->response->stage = "chunk-read";
            if (!read_exact(reader, *body, chunk, used)) return false;
            reader->response->stage = "chunk-terminator";
            n = read_line(reader, line);
            if (n < 0) return false;
            if (n) return fail(reader, "chunk-terminator", -RT_EINVAL);
        }
        for (;;)
        {
            char *value;
            reader->response->stage = "trailers-read";
            int n = header_line(reader, line, &total);
            if (n < 0) return false;
            if (!n) break;
            if (!split_header(line, &value) || equal(line, "content-length") ||
                equal(line, "transfer-encoding") || equal(line, "content-encoding"))
                return fail(reader, "trailers-read", -RT_EINVAL);
            if (!header_callback(reader, line, value)) return false;
        }
    }
    else if (has_length)
    {
        if (!body_reserve(reader, body, 0, &capacity, length, true)) return false;
        reader->response->stage = "body-read";
        if (!read_exact(reader, *body, length, used)) return false;
    }
    else
    {
        for (;;)
        {
            unsigned char buffer[IO_SLICE];
            reader->response->stage = "body-eof";
            int count = tls_read(reader, buffer, sizeof(buffer));
            /* Only authenticated TLS close_notify completes an EOF body. A
             * bare TCP FIN cannot distinguish a complete body from truncation. */
            if (count == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) break;
            if (count <= 0) return false;
            if ((size_t)count > reader->limit - *used) return fail(reader, "body-limit", -RT_EFULL);
            if (!body_reserve(reader, body, *used, &capacity, *used + (size_t)count, false)) return false;
            memcpy(*body + *used, buffer, (size_t)count);
            *used += (size_t)count;
        }
    }
    return body_reserve(reader, body, *used, &capacity, *used, true);
}

static bool gzip_decode(http_reader_t *reader, char **body, size_t *size)
{
    const unsigned char *src = (const unsigned char *)*body;
    size_t offset = 10, end, expected, output_used = 0;
    uint32_t expected_crc = 0, expected_size = 0;
    char *output = NULL;
    tinfl_decompressor *decoder = NULL;
    bool ok = false;
    reader->response->stage = "gzip-header";
    if (*size < 18 || src[0] != 0x1f || src[1] != 0x8b || src[2] != 8 || (src[3] & 0xe0))
        return fail(reader, "gzip-header", -RT_EINVAL);
    end = *size - 8;
    if (src[3] & 4)
    {
        if (end < 12) return fail(reader, "gzip-header", -RT_EINVAL);
        offset = 12 + src[10] + ((size_t)src[11] << 8);
    }
    for (unsigned flag = 8; flag <= 16; flag *= 2)
    {
        if (!(src[3] & flag)) continue;
        while (offset < end && src[offset])
        {
            if (expired(reader)) return false;
            ++offset;
        }
        if (offset >= end) return fail(reader, "gzip-header", -RT_EINVAL);
        ++offset;
    }
    if (offset >= end || ((src[3] & 2) && end - offset < 2))
        return fail(reader, "gzip-header", -RT_EINVAL);
    if (src[3] & 2)
    {
        uint32_t crc = 0;
        for (size_t i = 0; i < offset;)
        {
            size_t count = offset - i > IO_SLICE ? IO_SLICE : offset - i;
            if (expired(reader)) return false;
            crc = (uint32_t)mz_crc32(crc, src + i, count);
            i += count;
        }
        if ((crc & 0xffffu) != (src[offset] | ((uint32_t)src[offset + 1] << 8)))
            return fail(reader, "gzip-header-crc", -RT_EINVAL);
        offset += 2;
    }
    for (unsigned i = 0; i < 4; ++i)
    {
        expected_crc |= (uint32_t)src[end + i] << (8 * i);
        expected_size |= (uint32_t)src[end + 4 + i] << (8 * i);
    }
    expected = (size_t)expected_size;
    if (expected > reader->limit) return fail(reader, "body-limit", -RT_EFULL);
    if (expired(reader)) return false;
    output = epd_app_alloc(expected + 1, EPD_APP_PSRAM);
    if (!output) return fail(reader, "gzip-output-alloc", -RT_ENOMEM);
    decoder = epd_app_alloc(sizeof(*decoder), EPD_APP_PSRAM);
    if (!decoder) { fail(reader, "gzip-decoder-alloc", -RT_ENOMEM); goto finish; }
    tinfl_init(decoder);
    reader->response->stage = "gzip-inflate";
    uint32_t crc = 0;
    for (;;)
    {
        size_t input_size = end - offset;
        size_t output_size = expected + 1 - output_used;
        if (expired(reader)) goto finish;
        if (input_size > IO_SLICE) input_size = IO_SLICE;
        if (output_size > IO_SLICE) output_size = IO_SLICE;
        unsigned flags = TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF;
        if (input_size < end - offset) flags |= TINFL_FLAG_HAS_MORE_INPUT;
        tinfl_status result = tinfl_decompress(decoder, src + offset, &input_size,
            (unsigned char *)output, (unsigned char *)output + output_used, &output_size,
            flags);
        crc = (uint32_t)mz_crc32(crc, (unsigned char *)output + output_used, output_size);
        output_used += output_size;
        offset += input_size;
        if (output_used > expected) { fail(reader, "gzip-length", -RT_EINVAL); goto finish; }
        if (result == TINFL_STATUS_DONE) break;
        if ((result != TINFL_STATUS_HAS_MORE_OUTPUT && result != TINFL_STATUS_NEEDS_MORE_INPUT) ||
            (!input_size && !output_size))
        { fail(reader, "gzip-inflate", (int)result); goto finish; }
    }
    if (output_used != expected || offset != end) { fail(reader, "gzip-length", -RT_EINVAL); goto finish; }
    if (crc != expected_crc) { fail(reader, "gzip-crc", -RT_EINVAL); goto finish; }
    if (expired(reader)) goto finish;
    output[output_used] = '\0';
    epd_app_free(*body);
    *body = output;
    *size = output_used;
    output = NULL;
    ok = true;
finish:
    epd_app_free(decoder);
    epd_app_free(output);
    return ok;
}

void epd_http_response_clear(epd_http_response_t *response)
{
    if (!response) return;
    epd_app_free(response->body);
    memset(response, 0, sizeof(*response));
}

bool epd_http_perform(const epd_http_request_t *request, epd_http_response_t *response)
{
    http_reader_t reader = {0};
    char *line = NULL, *body = NULL;
    size_t used = 0;
    bool gzip = false, complete = false;
    if (!response) return false;
    memset(response, 0, sizeof(*response));
    reader.response = response;
    reader.request = request;
    if (!request) return fail(&reader, "request-invalid", -RT_EINVAL);
    reader.start = rt_tick_get();
    uint32_t timeout = request->timeout_ms ? request->timeout_ms : 30000u;
    reader.timeout_ticks = ((uint64_t)timeout * RT_TICK_PER_SECOND + 999u) / 1000u;
    /* Unsigned tick subtraction must remain representable through one wrap. */
    if (reader.timeout_ticks > (rt_tick_t)-1) return fail(&reader, "request-timeout", -RT_EINVAL);
    reader.limit = request->body_limit && request->body_limit < SIZE_MAX ? request->body_limit : SIZE_MAX - 1;
    network_snapshot(&reader.route);
    if (expired(&reader) || !request_valid(&reader)) return false;
    line = epd_app_alloc(LINE_SIZE, EPD_APP_PSRAM);
    if (!line) return fail(&reader, "headers-alloc", -RT_ENOMEM);
    reader.tls = rt_calloc(1, sizeof(*reader.tls));
    if (!reader.tls) { fail(&reader, "tls-session-alloc", -RT_ENOMEM); goto finish; }
    response->stage = "tls-init";
    response->detail = mbedtls_client_init(reader.tls, "epd-http", 8);
    if (response->detail) goto finish;
    reader.tls->host = rt_strdup(request->host);
    if (!reader.tls->host) { fail(&reader, "tls-host-alloc", -RT_ENOMEM); goto finish; }
    response->stage = "tls-context";
    response->detail = mbedtls_client_context(reader.tls);
    if (response->detail || expired(&reader)) goto finish;
    if (reader.tls->conf.authmode != MBEDTLS_SSL_VERIFY_REQUIRED)
    { fail(&reader, "tls-authmode", -RT_ERROR); goto finish; }
    if (!tls_connect(&reader) || !send_request(&reader)) goto finish;
    complete = receive_body(&reader, line, &body, &used, &gzip) && !expired(&reader);
finish:
    epd_app_free(line);
    /* Release TLS record buffers before allocating the gzip decoder/output. */
    if (reader.tls) mbedtls_client_close(reader.tls);
    if (complete)
    {
        body[used] = '\0';
        if (gzip || (used >= 2 && (unsigned char)body[0] == 0x1f && (unsigned char)body[1] == 0x8b))
            complete = gzip_decode(&reader, &body, &used);
        if (complete) complete = !expired(&reader);
    }
    if (!complete)
    {
        epd_app_free(body);
        if (!response->detail) response->detail = -RT_ERROR;
        return false;
    }
    response->body = body;
    response->size = used;
    response->stage = "complete";
    response->detail = 0;
    return true;
}
