/* SPDX-License-Identifier: Apache-2.0 */
#include "weather_http.h"
#include "../common/http/epd_http.h"
#include <string.h>

#define DBG_TAG "weather.http"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

static bool cancelled(void *user)
{
    (void)user;
    return weather_cancelled();
}

char *weather_http_get(const weather_config_t *config, const char *path, int *status)
{
    epd_http_response_t response = {0};
    char *body = NULL;
    if (status) *status = 0;
    if (!config || !status) return NULL;
    const epd_http_header_t headers[] = {
        {"X-QW-Api-Key", config->api_key},
        {"Accept", "application/json"},
        {"Accept-Encoding", "gzip"},
    };
    const epd_http_request_t request = {
        .host = config->api_host,
        .path = path,
        .method = "GET",
        .headers = headers,
        .header_count = sizeof(headers) / sizeof(headers[0]),
        .timeout_ms = 30000,
        .body_limit = 32768,
        .cancelled = cancelled,
    };
    bool complete = epd_http_perform(&request, &response);
    *status = response.status;
    if (complete && response.status == 200 && response.size && !weather_cancelled())
    {
        /* Preserve the weather API's rt_malloc/rt_free body ownership. */
        body = rt_malloc(response.size + 1);
        if (body) memcpy(body, response.body, response.size + 1);
        else LOG_E("stage=weather-body-alloc bytes=%u", (unsigned)(response.size + 1));
    }
    else
        LOG_E("stage=%s HTTP=%d detail=%d verify=%08x",
              response.stage ? response.stage : "request", response.status,
              response.detail, (unsigned)response.verify_flags);
    epd_http_response_clear(&response);
    return body;
}
