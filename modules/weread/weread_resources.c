#include "weread_resources.h"
#include "../common/http/epd_http.h"
#include <libxml/uri.h>
#include <string.h>
#include <stdio.h>

typedef struct { char *data; size_t size, offset; } image_stream_t;
typedef struct
{
    const weread_result_t *content;
    bool (*cancel)(void *);
    void *user;
} resource_request_t;

static bool cancelled(void *user)
{
    const resource_request_t *request = user;
    return weread_unavailable() || (request->cancel ? request->cancel(request->user) :
        weread_cancelled(request->content->generation));
}

static int image_read(void *user, void *buffer, size_t length)
{
    image_stream_t *image = user;
    if (length > image->size - image->offset) length = image->size - image->offset;
    if (length > INT_MAX) length = INT_MAX;
    memcpy(buffer, image->data + image->offset, length);
    image->offset += length;
    return (int)length;
}

static int image_seek(void *user, uint64_t offset)
{
    image_stream_t *image = user;
    if (offset > image->size) return BOOK_IO_ERROR;
    image->offset = (size_t)offset;
    return BOOK_OK;
}

static void image_close(void *user)
{
    image_stream_t *image = user;
    epd_app_free(image->data); epd_app_free(image);
}

int weread_resource_open(void *user, const char *base, const char *reference, book_stream_t *stream)
{
    return weread_resource_open_ex(user, base, reference, stream, NULL, NULL);
}

int weread_resource_open_ex(const weread_result_t *content, const char *base,
    const char *reference, book_stream_t *stream, bool (*cancel)(void *), void *cancel_user)
{
    resource_request_t context = {content, cancel, cancel_user};
    void *user = &context;
    if (!stream) return BOOK_ERROR;
    memset(stream, 0, sizeof(*stream));
    if (!content) return BOOK_ERROR;
    if (!reference || cancelled(user)) return BOOK_CANCELLED;
    const char *origin = base && !strncmp(base, "https://", 8) ? base : "https://weread.qq.com/";
    xmlChar *resolved = xmlBuildURI((const xmlChar *)reference, (const xmlChar *)origin);
    if (!resolved) return BOOK_NO_MEMORY;
    char *url = (char *)resolved;
    if (strncmp(url, "https://", 8)) { xmlFree(resolved); return BOOK_UNSUPPORTED; }
    char *start = url + 8, *slash = strchr(start, '/');
    size_t host_size = slash ? (size_t)(slash - start) : strlen(start);
    char *host = epd_app_alloc(host_size + 1, EPD_APP_PSRAM);
    if (!host) { xmlFree(resolved); return BOOK_NO_MEMORY; }
    memcpy(host, start, host_size); host[host_size] = 0;
    char key[48];
    size_t url_size = strlen(url) + 1;
    snprintf(key, sizeof(key), "image-%08lx", (unsigned long)storage_crc32(url, url_size));
    size_t size = 0;
    char *cached = weread_store_load(STORAGE_APP_CACHE, content->account, content->book, key, &size);
    char *data = NULL;
    if (cached && size > url_size && !memcmp(cached, url, url_size))
    {
        size -= url_size;
        memmove(cached, cached + url_size, size);
        data = cached;
    }
    else epd_app_free(cached);
    int status = BOOK_OK;
    if (!data)
    {
        const epd_http_header_t headers[] = {{"Referer", "https://weread.qq.com/"}};
        epd_http_request_t request = {.host = host, .path = slash ? slash : "/", .method = "GET",
            .headers = headers, .header_count = 1, .cancelled = cancelled, .user = user};
        epd_http_response_t response = {0};
        if (!epd_http_perform(&request, &response) || response.status != 200)
            status = cancelled(user) ? BOOK_CANCELLED : BOOK_IO_ERROR;
        else
        {
            data = response.body; size = response.size; response.body = NULL;
            if (size <= SIZE_MAX - url_size && !cancelled(user))
            {
                char *record = epd_app_alloc(size + url_size, EPD_APP_PSRAM);
                if (record)
                {
                    memcpy(record, url, url_size); memcpy(record + url_size, data, size);
                    if (!cancelled(user))
                        weread_store_save(STORAGE_APP_CACHE, content->account, content->book, key, record, size + url_size);
                    epd_app_free(record);
                }
            }
        }
        epd_http_response_clear(&response);
    }
    epd_app_free(host); xmlFree(resolved);
    if (status != BOOK_OK || cancelled(user)) { epd_app_free(data); return cancelled(user) ? BOOK_CANCELLED : status; }
    image_stream_t *image = epd_app_alloc(sizeof(*image), EPD_APP_PSRAM);
    if (!image) { epd_app_free(data); return BOOK_NO_MEMORY; }
    if (cancelled(user)) { epd_app_free(image); epd_app_free(data); return BOOK_CANCELLED; }
    *image = (image_stream_t){data, size, 0};
    *stream = (book_stream_t){image, size, image_read, image_seek, image_close};
    return BOOK_OK;
}
