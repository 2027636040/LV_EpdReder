#ifndef WEREAD_COVERS_H
#define WEREAD_COVERS_H
#include "weread_service.h"

#define WEREAD_COVERS_MAX 8u
#define WEREAD_COVER_WIDTH 56u
#define WEREAD_COVER_HEIGHT 72u

typedef struct weread_cover
{
    uint32_t request;                 /* Token returned by covers_request. */
    uint32_t generation, revision;    /* Source snapshot identity. */
    size_t index;                     /* Absolute index in source shelf. */
    char *account, *book, *url;        /* Owned, including original cover URL. */
    lv_image_dsc_t image;             /* 16-level grayscale RGB565. */
    void *memory;                     /* Allocation base; image.data is aligned. */
} weread_cover_t;

/* UI only. Copies account and up to eight visible book IDs/cover URLs before
 * returning; does not retain or modify snapshot. Replaces queued/in-flight
 * work and unclaimed output. Returns a nonzero request token, or zero for an
 * empty/invalid request or allocation failure (the old request is cancelled).
 * No download, decode or filesystem operation runs on this call. */
uint32_t weread_covers_request(const weread_result_t *snapshot, size_t first, size_t count);

/* The existing worker calls this serially. Processes at most one cover.
 * Returns true if an item was attempted, even when it failed. A single ready
 * result applies backpressure: step returns false until UI takes/discards it.
 * Failed/empty covers are skipped; the shelf text remains independently usable. */
bool weread_covers_step(void);

/* UI only. Transfers one completed object, or NULL. Cleans the aligned pixel
 * buffer before returning and wakes the worker to process the next item.
 * The UI matches request/generation/revision/index before binding &image. */
weread_cover_t *weread_covers_take(void);
/* UI only. Detach the image descriptor from its widget before calling free.
 * Waits for drawing, drops LVGL descriptor caches, then releases the object. */
void weread_covers_free(weread_cover_t *cover);

/* UI only. Cancels page work without cancelling the service generation.
 * Queued/ready allocations are released immediately. A worker-owned in-flight
 * item observes cancellation and releases its own resources before step exits.
 * Objects already transferred by take remain UI-owned and must be freed there. */
void weread_covers_cancel(void);
/* Worker exit, after the last step has returned and service admission closed.
 * Releases all queue-owned resources. UI-owned taken objects must be detached
 * and freed before module unload. Does not wait or create/stop threads. */
void weread_covers_shutdown(void);
#endif
