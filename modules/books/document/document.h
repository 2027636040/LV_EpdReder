/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BOOK_DOCUMENT_H
#define BOOK_DOCUMENT_H
#include "../bookshelf.h"
#include "../formats/formats.h"
#include "platform/epd_app.h"

#define DOCUMENT_TEXT_LIMIT 1024u
#define DOCUMENT_IMAGE_WIDTH 620u
#define DOCUMENT_IMAGE_HEIGHT 1040u
/* Bump whenever converter ordering, normalization, image sizing or spool
 * record construction changes. Positions refer to this content model. */
#define DOCUMENT_CONVERSION_VERSION 2u

typedef book_format_t document_format_t;
#define DOCUMENT_UNKNOWN BOOK_FORMAT_UNKNOWN
#define DOCUMENT_TXT BOOK_FORMAT_TXT
#define DOCUMENT_MD BOOK_FORMAT_MARKDOWN
#define DOCUMENT_EPUB BOOK_FORMAT_EPUB
#define DOCUMENT_SPLIT_EPUB BOOK_FORMAT_SPLIT_EPUB
#define DOCUMENT_MOBI BOOK_FORMAT_MOBI
typedef enum { DOCUMENT_TEXT = 1, DOCUMENT_PARAGRAPH, DOCUMENT_ANCHOR,
               DOCUMENT_LINK, DOCUMENT_IMAGE } document_record_kind_t;
typedef struct { uint32_t kind, length, style, link; } document_record_t;
typedef struct { uint32_t width, height; } document_image_t;
typedef struct { uint32_t record, byte; } document_cursor_t;
typedef struct document document_t;
typedef struct {
    uint32_t end, publication;
    bool complete, stopped;
    int result;
    char title[BOOKSHELF_NAME_MAX], error[96];
} document_status_t;
typedef struct {
    lv_image_dsc_t image;
    void *memory;
} document_bitmap_t;

document_format_t document_format(const char *path, bool directory);
const char *document_format_name(document_format_t format);
/* The caller holds storage_lock during directory discovery. */
bool document_split_identity(const char *path, uint32_t *size, uint32_t *signature);
void document_worker(epd_service_t *service);
void document_service_process(void);
void document_service_shutdown(void);
document_t *document_open(const bookshelf_item_t *file, char *error, size_t size);
void document_close(document_t *document);
void document_status(document_t *document, document_status_t *status);
bool document_read(document_t *document, uint32_t offset, void *data, size_t length);
bool document_record(document_t *document, uint32_t offset, document_record_t *record);
/* UI-owned RGB565 output; its descriptor stays alive until the widget detaches. */
bool document_image_load(document_t *document, uint32_t offset, unsigned width,
                         unsigned height, document_bitmap_t *bitmap);

/* Synchronous worker-only image conversion, with bounded source and target sizes. */
int document_image_decode(book_stream_t *stream, bool (*cancelled)(void *), void *user,
                          uint8_t **gray, unsigned *width, unsigned *height);
#endif
