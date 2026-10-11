#include "weread_covers.h"
#include "weread_resources.h"
#include "../books/image/book_image.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "src/misc/cache/instance/lv_image_header_cache.h"
#include <rthw.h>
#include <string.h>

typedef struct { char *book, *url; size_t index; } cover_item_t;
typedef struct
{
    unsigned references;
    bool cancelled;
    uint32_t request, revision;
    size_t count, next;
    weread_result_t context; /* Private resource/cache context, never the UI snapshot. */
    cover_item_t items[WEREAD_COVERS_MAX];
} cover_job_t;

/* Only pointer/counter operations run with interrupts disabled. The module's
 * UI thread submits/takes; its one managed worker owns all I/O and decoding. */
static cover_job_t *pending_cover, *active_cover;
static weread_cover_t *ready_cover;
static uint32_t cover_serial;

static char *cover_string(const char *text)
{
    size_t size = strlen(text) + 1;
    char *copy = epd_app_alloc(size, EPD_APP_PSRAM);
    if (copy) memcpy(copy, text, size);
    return copy;
}

static void cover_dispose(weread_cover_t *cover)
{
    if (!cover) return;
    epd_app_free(cover->memory);
    epd_app_free(cover->account);
    epd_app_free(cover->book);
    epd_app_free(cover->url);
    epd_app_free(cover);
}

static void cover_job_release(cover_job_t *job)
{
    if (!job) return;
    rt_base_t level = rt_hw_interrupt_disable();
    bool last = --job->references == 0;
    rt_hw_interrupt_enable(level);
    if (!last) return;
    for (size_t i = 0; i < job->count; ++i)
    {
        epd_app_free(job->items[i].book);
        epd_app_free(job->items[i].url);
    }
    epd_app_free(job->context.account);
    epd_app_free(job);
}

static uint32_t cover_replace(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    cover_job_t *old = pending_cover;
    weread_cover_t *output = ready_cover;
    pending_cover = NULL;
    ready_cover = NULL;
    if (old) old->cancelled = true;
    if (active_cover) active_cover->cancelled = true;
    if (!++cover_serial) ++cover_serial;
    uint32_t serial = cover_serial;
    rt_hw_interrupt_enable(level);
    cover_job_release(old);
    cover_dispose(output); /* An untaken descriptor has never reached LVGL. */
    return serial;
}

static bool cover_cancelled(void *user)
{
    cover_job_t *job = user;
    rt_base_t level = rt_hw_interrupt_disable();
    bool cancelled = job->cancelled || job->request != cover_serial;
    rt_hw_interrupt_enable(level);
    return cancelled || weread_cancelled(job->context.generation);
}

uint32_t weread_covers_request(const weread_result_t *snapshot, size_t first, size_t count)
{
    uint32_t serial = cover_replace();
    if (!snapshot || !snapshot->account || !snapshot->account[0] || !snapshot->books ||
        first >= snapshot->book_count || !count || weread_cancelled(snapshot->generation))
    { weread_wake(); return 0; }
    if (count > WEREAD_COVERS_MAX) count = WEREAD_COVERS_MAX;
    if (count > snapshot->book_count - first) count = snapshot->book_count - first;
    cover_job_t *job = epd_app_alloc(sizeof(*job), EPD_APP_PSRAM);
    if (!job) { weread_wake(); return 0; }
    memset(job, 0, sizeof(*job));
    job->references = 1;
    job->request = serial;
    job->revision = snapshot->revision;
    job->context.generation = snapshot->generation;
    job->context.account = cover_string(snapshot->account);
    if (!job->context.account) goto failed;
    for (size_t i = 0; i < count; ++i)
    {
        const wr_book_t *book = &snapshot->books[first + i];
        if (!book->id || !book->id[0] || !book->cover || !book->cover[0]) continue;
        cover_item_t *item = &job->items[job->count++];
        item->index = first + i;
        item->book = cover_string(book->id);
        item->url = cover_string(book->cover);
        if (!item->book || !item->url) goto failed;
    }
    if (!job->count || cover_cancelled(job)) goto failed;
    rt_base_t level = rt_hw_interrupt_disable();
    bool accepted = serial == cover_serial;
    if (accepted) pending_cover = job;
    rt_hw_interrupt_enable(level);
    if (accepted) { weread_wake(); return serial; }
failed:
    cover_job_release(job);
    weread_wake();
    return 0;
}

static weread_cover_t *cover_decode(cover_job_t *job, const cover_item_t *item)
{
    book_stream_t stream = {0};
    uint8_t *gray = NULL;
    unsigned width = 0, height = 0;
    weread_cover_t *cover = NULL;
    job->context.book = item->book;
    if (cover_cancelled(job)) return NULL;
    int status = weread_resource_open_ex(&job->context, "https://weread.qq.com/",
        item->url, &stream, cover_cancelled, job);
    if (status == BOOK_OK && !cover_cancelled(job))
    {
        char error[96];
        status = book_image_decode(&stream, WEREAD_COVER_WIDTH, WEREAD_COVER_HEIGHT,
            cover_cancelled, job, &gray, &width, &height, error, sizeof(error));
    }
    if (stream.close) stream.close(stream.user);
    if (status != BOOK_OK || !gray || !width || !height ||
        width > WEREAD_COVER_WIDTH || height > WEREAD_COVER_HEIGHT || cover_cancelled(job)) goto done;
    cover = epd_app_alloc(sizeof(*cover), EPD_APP_PSRAM);
    if (!cover) goto done;
    memset(cover, 0, sizeof(*cover));
    cover->request = job->request;
    cover->generation = job->context.generation;
    cover->revision = job->revision;
    cover->index = item->index;
    cover->account = cover_string(job->context.account);
    cover->book = cover_string(item->book);
    cover->url = cover_string(item->url);
    unsigned stride = RT_ALIGN(width * 2, 4);
    cover->memory = epd_app_alloc(stride * height + 31, EPD_APP_PSRAM);
    if (!cover->account || !cover->book || !cover->url || !cover->memory) goto failed;
    uint8_t *data = (uint8_t *)RT_ALIGN((uintptr_t)cover->memory, 32);
    memset(data, 0xff, stride * height);
    cover->image.header.magic = LV_IMAGE_HEADER_MAGIC;
    cover->image.header.cf = LV_COLOR_FORMAT_RGB565;
    cover->image.header.w = width;
    cover->image.header.h = height;
    cover->image.header.stride = stride;
    cover->image.data = data;
    cover->image.data_size = stride * height;
    for (unsigned y = 0; y < height; ++y)
    {
        if (cover_cancelled(job)) goto failed;
        uint16_t *row = (uint16_t *)(data + y * stride);
        for (unsigned x = 0; x < width; ++x)
        {
            unsigned value = gray[y * width + x];
            row[x] = (uint16_t)(((value & 0xf8) << 8) | ((value & 0xfc) << 3) | (value >> 3));
        }
    }
    goto done;
failed:
    cover_dispose(cover);
    cover = NULL;
done:
    epd_app_free(gray);
    return cover;
}

bool weread_covers_step(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    cover_job_t *job = pending_cover;
    if (!job || active_cover || ready_cover)
    { rt_hw_interrupt_enable(level); return false; }
    const cover_item_t *item = &job->items[job->next++];
    ++job->references;
    active_cover = job;
    bool last = job->next == job->count;
    if (last) pending_cover = NULL;
    rt_hw_interrupt_enable(level);
    if (last) cover_job_release(job);
    weread_cover_t *cover = cover_decode(job, item);
    bool cancelled = cover_cancelled(job);
    level = rt_hw_interrupt_disable();
    active_cover = NULL;
    bool publish = cover && !cancelled && !job->cancelled && job->request == cover_serial;
    if (publish) ready_cover = cover;
    bool discard_queue = cancelled && pending_cover == job;
    if (discard_queue) pending_cover = NULL;
    rt_hw_interrupt_enable(level);
    if (!publish) cover_dispose(cover);
    if (discard_queue) cover_job_release(job);
    cover_job_release(job);
    return true;
}

weread_cover_t *weread_covers_take(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    weread_cover_t *cover = ready_cover;
    ready_cover = NULL;
    rt_hw_interrupt_enable(level);
    if (!cover) return NULL;
    if (weread_cancelled(cover->generation)) { cover_dispose(cover); cover = NULL; }
    if (cover)
    {
        lv_draw_buf_t buffer = {.data = (uint8_t *)cover->image.data, .data_size = cover->image.data_size};
        epd_app_clean_draw_buffer(&buffer);
    }
    weread_wake();
    return cover;
}

void weread_covers_free(weread_cover_t *cover)
{
    if (!cover) return;
    lv_draw_wait_for_finish();
    lv_image_cache_drop(&cover->image);
    lv_image_header_cache_drop(&cover->image);
    cover_dispose(cover);
}

void weread_covers_cancel(void)
{
    cover_replace();
    weread_wake();
}

void weread_covers_shutdown(void)
{
    cover_replace();
}
