#ifndef HTTP_TEST_FAKE_TRANSPORT_H
#define HTTP_TEST_FAKE_TRANSPORT_H
#include "epd_http.h"
#include "rtthread.h"

typedef struct
{
    const unsigned char *wire;
    size_t wire_size, received, fragment, write_fragment;
    unsigned char sent[131072];
    size_t sent_size;
    unsigned allocations, fail_allocation, live, psram_allocations;
    size_t max_allocation;
    unsigned dns_calls, sockets, closes, handshake_calls, write_calls, read_calls;
    unsigned snapshot_calls, cancel_at_snapshot, generation_at_snapshot;
    unsigned select_calls, select_waits, handshake_waits, write_waits, read_waits;
    uint32_t ticks, generation, dns_delay, verify_flags;
    int dns_error, connect_error, socket_error, init_error, context_error;
    int handshake_error, read_error, write_error, eof, authmode;
    bool ready, cancelled, generation_in_dns, cancel_in_dns, weather_cancel;
    uint32_t bound_address;
    const char *cancel_stage;
    epd_http_response_t *observed_response;
} fake_transport_t;
extern fake_transport_t fake;
void fake_reset(const void *wire, size_t size);
void fake_reset_text(const char *wire);
bool fake_cancelled(void *user);
void fake_assert_clean(void);
#endif
