/* SPDX-License-Identifier: Apache-2.0 */
#include "fake_transport.h"
#include "weather_http.h"
#include <tls_client.h>
#include <miniz.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
static const char response_ok[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}";

static epd_http_request_t request_default(void)
{
    epd_http_request_t request = {0};
    request.host = "api.example.com";
    request.path = "/path?q=1";
    request.cancelled = fake_cancelled;
    request.user = &fake;
    return request;
}

static void perform_expect(epd_http_request_t *request, bool success, const char *body, size_t size)
{
    epd_http_response_t response = {0};
    fake.observed_response = &response;
    bool result = epd_http_perform(request, &response);
    if (result != success)
        fprintf(stderr, "case %u stage=%s detail=%d status=%d expected=%d\n",
                checks + 1, response.stage, response.detail, response.status, success);
    assert(result == success);
    if (success)
    {
        assert(response.body && response.size == size && response.body[size] == 0);
        assert(!size || !memcmp(response.body, body, size));
        assert(!strcmp(response.stage, "complete") && !response.detail);
    }
    else assert(!response.body && !response.size && response.stage && response.detail);
    epd_http_response_clear(&response);
    epd_http_response_clear(&response);
    fake.observed_response = NULL;
    fake_assert_clean();
    ++checks;
}

static void test_hosts(void)
{
    const char *valid[] = {"api.example.com", "API.Example.COM", "localhost", "127.0.0.1",
                          "255.255.255.255", "xn--bcher-kva.example", "example.com."};
    const char *invalid[] = {NULL, "", "https://example.com", "example.com:443", "user@example.com",
        "a/b", "a\\b", "a\r\nHost: x", "a b", ".example", "example..com", "-a.com", "a-.com",
        "a_b.com", "[::1]", "256.1.1.1", "127.1", "1.2.3.4.5", "1.2.3.4.", "1234"};
    epd_http_request_t request = request_default();
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i)
    {
        fake_reset_text(response_ok);
        request.host = valid[i];
        perform_expect(&request, true, "{}", 2);
        assert(fake.dns_calls == 1 && fake.bound_address == 0x01020304);
    }
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
    {
        fake_reset_text(response_ok);
        request.host = invalid[i];
        perform_expect(&request, false, NULL, 0);
        assert(!fake.dns_calls && !fake.sent_size && !fake.allocations);
    }
    char label[65];
    memset(label, 'a', 64); label[64] = 0;
    fake_reset_text(response_ok); request.host = label;
    perform_expect(&request, false, NULL, 0);
    label[63] = 0;
    fake_reset_text(response_ok);
    perform_expect(&request, true, "{}", 2);
    puts("host validation: DNS labels, IPv4, authority and injection rejection passed");
}

static unsigned cookies, dates;
static bool collect_header(void *user, const char *name, const char *value)
{
    assert(user == &fake);
    if (!strcmp(name, "Set-Cookie"))
    {
        assert(!strcmp(value, cookies ? "B=Two; Path=/; HttpOnly" : "A=One; Path=/"));
        ++cookies;
    }
    if (!strcmp(name, "Date"))
    {
        assert(!strcmp(value, "Sun, 11 Oct 2026 00:00:00 GMT"));
        ++dates;
    }
    return true;
}

static bool reject_header(void *user, const char *name, const char *value)
{ (void)name; (void)value; assert(user == &fake); return false; }

static void test_requests_headers(void)
{
    epd_http_request_t request = request_default();
    const unsigned char body[] = {'a', 0, 'b'};
    epd_http_header_t headers[] = {{"Cookie", "session=AbC"}, {"X-Custom", "one"}, {"X-Custom", "two"}};
    request.method = "POST";
    request.headers = headers; request.header_count = 3;
    request.body = body; request.body_size = sizeof(body);
    request.on_header = collect_header;
    fake_reset_text("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\n"
        "Set-Cookie: A=One; Path=/\r\nSet-Cookie: B=Two; Path=/; HttpOnly\r\n"
        "Date: Sun, 11 Oct 2026 00:00:00 GMT  \t\r\nContent-Length: 2\r\n\r\n{}");
    fake.write_waits = 2; fake.handshake_waits = 2; fake.read_waits = 2; fake.select_waits = 1;
    perform_expect(&request, true, "{}", 2);
    assert(cookies == 2 && dates == 1 && fake.select_calls == 2);
    assert(strstr((char *)fake.sent, "POST /path?q=1 HTTP/1.1\r\nHost: api.example.com\r\n"));
    assert(strstr((char *)fake.sent, "Cookie: session=AbC\r\nX-Custom: one\r\nX-Custom: two\r\n"));
    assert(strstr((char *)fake.sent, "Content-Length: 3\r\n\r\na"));
    assert(!memcmp(fake.sent + fake.sent_size - sizeof(body), body, sizeof(body)));
    request = request_default(); request.method = "POST";
    fake_reset_text(response_ok); perform_expect(&request, true, "{}", 2);
    assert(strstr((char *)fake.sent, "Content-Length: 0\r\n"));
    fake_reset_text(response_ok); fake.write_fragment = sizeof(fake.sent);
    perform_expect(&request, true, "{}", 2); assert(fake.write_calls == 1);
    request.method = "PUT";
    fake_reset_text(response_ok); perform_expect(&request, false, NULL, 0);
    request = request_default(); request.path = "/x\r\nBad: 1";
    fake_reset_text(response_ok); perform_expect(&request, false, NULL, 0);
    request = request_default(); request.body_size = 1;
    fake_reset_text(response_ok); perform_expect(&request, false, NULL, 0);
    request = request_default(); request.header_count = 1;
    fake_reset_text(response_ok); perform_expect(&request, false, NULL, 0);
    const char *reserved[] = {"host", "Connection", "CONTENT-LENGTH", "Transfer-Encoding", "Expect", "Bad Name", "X\r\n"};
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); ++i)
    {
        request = request_default(); request.headers = headers; request.header_count = 1;
        headers[0].name = reserved[i];
        fake_reset_text(response_ok); perform_expect(&request, false, NULL, 0);
        assert(!fake.dns_calls && !fake.sent_size);
    }
    headers[0].name = "X-Key"; headers[0].value = "ok\r\nHost: evil";
    fake_reset_text(response_ok); perform_expect(&request, false, NULL, 0);
    request = request_default(); request.on_header = reject_header;
    fake_reset_text(response_ok); perform_expect(&request, false, NULL, 0);
    puts("requests and headers: GET/POST, binary body, duplicates, Date, injection and callback cancellation passed");
}

static void test_framing(void)
{
    epd_http_request_t request = request_default();
    const char *good[] = {
        response_ok,
        "HTTP/1.0 200 OK\r\nContent-Length: 2\r\ncontent-length: 2\r\n\r\n{}",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: ChUnKeD\r\n\r\n1;x=y\r\n{\r\n1\r\n}\r\n0\r\nX-Trailer: Ok\r\n\r\n",
        "HTTP/1.1 200 OK\r\n\r\n{}",
        "HTTP/1.1 404 Missing\r\nContent-Length: 2\r\n\r\n{}",
        "HTTP/1.1 302 Found\r\nLocation: https://elsewhere.example/x\r\nContent-Length: 2\r\n\r\n{}",
    };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); ++i)
    {
        fake_reset_text(good[i]); fake.fragment = 1;
        perform_expect(&request, true, "{}", 2);
        assert(fake.dns_calls == 1 && fake.sockets == 1);
    }
    const char *empty[] = {
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n",
        "HTTP/1.1 204 No Content\r\n\r\n",
        "HTTP/1.1 304 Not Modified\r\nContent-Length: 99999\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
    };
    for (size_t i = 0; i < sizeof(empty) / sizeof(empty[0]); ++i)
    { fake_reset_text(empty[i]); perform_expect(&request, true, "", 0); }
    const char *bad[] = {
        "HTTP/1.1 200 OK\nContent-Length: 2\r\n\r\n{}",
        "HTTP/1.1 20x OK\r\n\r\n",
        "HTTP/1.1 2000 OK\r\n\r\n",
        "HTTP/1.1 101 Switching Protocols\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 2x\r\n\r\n{}",
        "HTTP/1.1 200 OK\r\nContent-Length: 999999999999999999999999999999999\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 3\r\n\r\n{}",
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nTransfer-Encoding: chunked\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Encoding: br\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Encoding: gzip\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length : 2\r\n\r\n{}",
        "HTTP/1.1 200 OK\r\n Bad: folded\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\n{}",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n-1\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1x\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nfffffffffffffffffffffffff\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\n{",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\n{}X\r\n0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\n{}\r\n0\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nContent-Length: 0\r\n\r\n",
        "HTTP/1.1 204 No Content\r\nContent-Length: 1\r\n\r\nx",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
    { fake_reset_text(bad[i]); perform_expect(&request, false, NULL, 0); }
    for (size_t length = 0; length < strlen(response_ok); ++length)
    { fake_reset(response_ok, length); perform_expect(&request, false, NULL, 0); }
    fake_reset_text("HTTP/1.1 200 OK\r\n\r\n{}"); fake.eof = 0;
    perform_expect(&request, false, NULL, 0);
    fake_reset_text("HTTP/1.1 200 OK\r\n\r\n{}"); fake.eof = -123;
    perform_expect(&request, false, NULL, 0);
    request.body_limit = 1;
    for (size_t i = 0; i < 4; ++i)
    { fake_reset_text(good[i]); perform_expect(&request, false, NULL, 0); }
    request.body_limit = 2;
    for (size_t i = 0; i < 4; ++i)
    { fake_reset_text(good[i]); perform_expect(&request, true, "{}", 2); }
    char huge[128];
    snprintf(huge, sizeof(huge), "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n", SIZE_MAX);
    request.body_limit = 0; fake_reset_text(huge); perform_expect(&request, false, NULL, 0);
    assert(fake.max_allocation < 10000);
    puts("response framing: status, interim, lengths, chunks, trailers, limits and authenticated EOF passed");
}

static void test_transport_faults(void)
{
    epd_http_request_t request = request_default();
    fake_reset_text(response_ok); fake.verify_flags = 0x100;
    epd_http_response_t response = {0};
    assert(!epd_http_perform(&request, &response));
    assert(response.verify_flags == 0x100 && !fake.sent_size && !strcmp(response.stage, "tls-verify"));
    epd_http_response_clear(&response); fake_assert_clean(); ++checks;
    fake_reset_text(response_ok); fake.authmode = 1;
    perform_expect(&request, false, NULL, 0); assert(!fake.dns_calls && !fake.sent_size);
    int *errors[] = {&fake.dns_error, &fake.connect_error, &fake.init_error, &fake.context_error,
                    &fake.handshake_error, &fake.read_error, &fake.write_error};
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i)
    { fake_reset_text(response_ok); *errors[i] = 123; if (i >= 4) *errors[i] = -123;
      perform_expect(&request, false, NULL, 0); }
    fake_reset_text(response_ok); fake.select_waits = 1; fake.socket_error = ECONNREFUSED;
    perform_expect(&request, false, NULL, 0);
    fake_reset_text(response_ok); fake.generation_in_dns = true;
    perform_expect(&request, false, NULL, 0); assert(!fake.sockets);
    fake_reset_text(response_ok); fake.cancel_in_dns = true;
    perform_expect(&request, false, NULL, 0); assert(!fake.sockets);
    fake_reset_text(response_ok); fake.ready = false;
    perform_expect(&request, false, NULL, 0); assert(!fake.dns_calls);
    fake_reset_text(response_ok); fake.cancelled = true;
    perform_expect(&request, false, NULL, 0); assert(!fake.dns_calls);
    request.timeout_ms = 20;
    fake_reset_text(response_ok); fake.dns_delay = 25;
    perform_expect(&request, false, NULL, 0); assert(!fake.sockets);
    unsigned *waits[] = {&fake.select_waits, &fake.handshake_waits, &fake.write_waits, &fake.read_waits};
    for (size_t i = 0; i < sizeof(waits) / sizeof(waits[0]); ++i)
    { fake_reset_text(response_ok); *waits[i] = 1000; perform_expect(&request, false, NULL, 0); }
    fake_reset_text(response_ok); fake.ticks = UINT32_MAX - 10; fake.handshake_waits = 3;
    perform_expect(&request, false, NULL, 0);
    request.timeout_ms = 0;
    const char *stages[] = {"dns", "tcp-connect", "tls-handshake", "request-write", "status-read", "headers-read", "body-read"};
    for (size_t i = 0; i < sizeof(stages) / sizeof(stages[0]); ++i)
    { fake_reset_text(response_ok); fake.cancel_stage = stages[i]; perform_expect(&request, false, NULL, 0); }
    fake_reset_text(response_ok); perform_expect(&request, true, "{}", 2);
    unsigned snapshots = fake.snapshot_calls;
    for (unsigned i = 1; i < snapshots; ++i)
    { fake_reset_text(response_ok); fake.cancel_at_snapshot = i; perform_expect(&request, false, NULL, 0); }
    for (unsigned i = 2; i <= snapshots; ++i)
    { fake_reset_text(response_ok); fake.generation_at_snapshot = i; perform_expect(&request, false, NULL, 0); }
    puts("transport faults: strict TLS, DNS/connect/I/O failures, timeouts, every cancellation/generation checkpoint passed");
}

static unsigned char *make_gzip(const void *plain, size_t size, size_t *wire_size)
{
    size_t compressed_size;
    unsigned char *compressed = tdefl_compress_mem_to_heap(plain, size, &compressed_size, TDEFL_DEFAULT_MAX_PROBES);
    assert(compressed);
    unsigned char *wire = malloc(compressed_size + 18);
    assert(wire);
    const unsigned char header[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 255};
    memcpy(wire, header, 10);
    memcpy(wire + 10, compressed, compressed_size);
    uint32_t crc = (uint32_t)mz_crc32(0, plain, size);
    for (unsigned i = 0; i < 4; ++i)
    {
        wire[10 + compressed_size + i] = (unsigned char)(crc >> (8 * i));
        wire[14 + compressed_size + i] = (unsigned char)(size >> (8 * i));
    }
    mz_free(compressed);
    *wire_size = compressed_size + 18;
    return wire;
}

static unsigned char *make_response(const void *body, size_t size, const char *encoding, size_t *wire_size)
{
    char header[160];
    int n = snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n%s\r\n", size, encoding);
    assert(n > 0 && n < (int)sizeof(header));
    *wire_size = (size_t)n + size;
    unsigned char *wire = malloc(*wire_size);
    assert(wire);
    memcpy(wire, header, (size_t)n);
    memcpy(wire + n, body, size);
    return wire;
}

static void test_gzip_allocation_weather(void)
{
    epd_http_request_t request = request_default();
    size_t gzip_size, wire_size;
    size_t plain_size = 70000;
    char *plain = malloc(plain_size);
    assert(plain);
    for (size_t i = 0; i < plain_size; ++i) plain[i] = (char)('a' + i % 19);
    unsigned char *gzip = make_gzip(plain, plain_size, &gzip_size);
    unsigned char *wire = make_response(gzip, gzip_size, "Content-Encoding: gzip\r\n", &wire_size);
    fake_reset(wire, wire_size); perform_expect(&request, true, plain, plain_size);
    unsigned allocations = fake.allocations;
    assert(fake.psram_allocations >= 4);
    for (unsigned i = 1; i <= allocations; ++i)
    { fake_reset(wire, wire_size); fake.fail_allocation = i; perform_expect(&request, false, NULL, 0); }
    fake_reset(wire, wire_size); fake.cancel_stage = "gzip-inflate";
    perform_expect(&request, false, NULL, 0);
    request.body_limit = 32768;
    fake_reset(wire, wire_size); perform_expect(&request, false, NULL, 0);
    request.body_limit = 0;
    free(wire);
    for (unsigned i = 0; i < 3; ++i)
    {
        size_t at = i == 0 ? gzip_size - 8 : i == 1 ? gzip_size - 4 : 3;
        gzip[at] ^= i == 2 ? 0x80 : 1;
        wire = make_response(gzip, gzip_size, "Content-Encoding: gzip\r\n", &wire_size);
        fake_reset(wire, wire_size); perform_expect(&request, false, NULL, 0);
        free(wire); gzip[at] ^= i == 2 ? 0x80 : 1;
    }
    for (size_t size = 0; size < gzip_size; ++size)
    {
        wire = make_response(gzip, size, "Content-Encoding: gzip\r\n", &wire_size);
        fake_reset(wire, wire_size); perform_expect(&request, false, NULL, 0);
        free(wire);
    }
    free(gzip);
    gzip = make_gzip("", 0, &gzip_size);
    wire = make_response(gzip, gzip_size, "Content-Encoding: gzip\r\n", &wire_size);
    fake_reset(wire, wire_size); perform_expect(&request, true, "", 0);
    free(gzip); free(wire);
    wire = make_response(plain, plain_size, "", &wire_size);
    fake_reset(wire, wire_size); perform_expect(&request, true, plain, plain_size);
    request.body_limit = 32768;
    fake_reset(wire, wire_size); perform_expect(&request, false, NULL, 0);
    weather_config_t config = {0};
    strcpy(config.api_host, "weather.example.com"); strcpy(config.api_key, "AbCd");
    int status;
    fake_reset(wire, wire_size);
    assert(!weather_http_get(&config, "/weather", &status) && status == 200); fake_assert_clean(); ++checks;
    free(wire); free(plain);
    fake_reset_text(response_ok);
    char *weather = weather_http_get(&config, "/weather", &status);
    assert(weather && status == 200 && !strcmp(weather, "{}")); rt_free(weather); fake_assert_clean(); ++checks;
    assert(strstr((char *)fake.sent, "GET /weather HTTP/1.1\r\n"));
    assert(strstr((char *)fake.sent, "X-QW-Api-Key: AbCd\r\nAccept: application/json\r\nAccept-Encoding: gzip\r\n"));
    unsigned weather_allocations = fake.allocations;
    for (unsigned i = 1; i <= weather_allocations; ++i)
    { fake_reset_text(response_ok); fake.fail_allocation = i;
      assert(!weather_http_get(&config, "/weather", &status)); fake_assert_clean(); ++checks; }
    const char *weather_bad[] = {"HTTP/1.1 404 Missing\r\nContent-Length: 2\r\n\r\n{}",
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"};
    for (size_t i = 0; i < 2; ++i)
    { fake_reset_text(weather_bad[i]); assert(!weather_http_get(&config, "/weather", &status)); fake_assert_clean(); ++checks; }
    fake_reset_text(response_ok); fake.weather_cancel = true;
    assert(!weather_http_get(&config, "/weather", &status) && !fake.dns_calls && !status); fake_assert_clean(); ++checks;
    gzip = make_gzip("{}", 2, &gzip_size);
    wire = make_response(gzip, gzip_size, "", &wire_size);
    fake_reset(wire, wire_size);
    weather = weather_http_get(&config, "/weather", &status);
    assert(weather && status == 200 && !strcmp(weather, "{}")); rt_free(weather); fake_assert_clean(); ++checks;
    free(wire); free(gzip);
    unsigned char noisy[4096];
    uint32_t random = 27;
    for (size_t i = 0; i < sizeof(noisy); ++i)
    { random = random * 1664525u + 1013904223u; noisy[i] = (unsigned char)(random >> 24); }
    gzip = make_gzip(noisy, sizeof(noisy), &gzip_size);
    assert(gzip_size > 1024);
    wire = make_response(gzip, gzip_size, "Content-Encoding: gzip\r\n", &wire_size);
    request.body_limit = 0;
    fake_reset(wire, wire_size); perform_expect(&request, true, (const char *)noisy, sizeof(noisy));
    free(wire); free(gzip);
    puts("gzip/memory/weather: real miniz, CRC/size/truncation, >32KiB, allocation failures and wrapper ownership passed");
}

int main(void)
{
    test_hosts();
    test_requests_headers();
    test_framing();
    test_transport_faults();
    test_gzip_allocation_weather();
    fake_assert_clean();
    printf("PASS: %u HTTP cases; no outstanding allocations or sockets\n", checks);
    return 0;
}
