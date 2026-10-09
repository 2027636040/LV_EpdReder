/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BOOK_CODEC_H
#define BOOK_CODEC_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BOOK_CODEC_ABI 1u
enum { BOOK_OK = 0, BOOK_ERROR = -1, BOOK_CANCELLED = -2,
       BOOK_NO_MEMORY = -3, BOOK_UNSUPPORTED = -4, BOOK_IO_ERROR = -5,
       BOOK_NO_SPACE = -6 };
enum { BOOK_STYLE_NORMAL = 0, BOOK_STYLE_BOLD = 1, BOOK_STYLE_ITALIC = 2,
       BOOK_STYLE_PREFORMAT = 4, BOOK_STYLE_CENTER = 8, BOOK_STYLE_RIGHT = 16,
       BOOK_STYLE_HEADING = 32, BOOK_STYLE_LINK = 64 };

/* Input streams are owned by their opener. A reader returns bytes, zero at EOF,
 * or a negative BOOK_* result. Optional seek uses an absolute byte offset. */
typedef struct book_stream
{
    void *user;
    uint64_t size;
    int (*read)(void *user, void *buffer, size_t length);
    int (*seek)(void *user, uint64_t offset);
    void (*close)(void *user);
} book_stream_t;

typedef struct book_resources
{
    void *user;
    int (*open)(void *user, const char *base, const char *reference, book_stream_t *stream);
} book_resources_t;

typedef struct book_sink book_sink_t;
struct book_sink
{
    void *user;
    bool (*cancelled)(void *user);
    /* These callbacks consume/copy their arguments before returning. */
    int (*text)(void *user, const char *utf8, size_t length, unsigned style);
    int (*paragraph)(void *user);
    int (*anchor)(void *user, const char *id, const char *title, unsigned level);
    int (*image)(void *user, const char *id, book_stream_t *stream);
    int (*metadata)(void *user, const char *title, const char *author);
    /* Fragment identifiers are resolved by the source adapter. */
    int (*link)(void *user, const char *target, bool begin);
    /* Import XHTML using the common parser, with module-owned resources. */
    int (*markup)(const book_sink_t *sink, book_stream_t *stream,
                  const char *base, const book_resources_t *resources);
};

typedef struct book_allocator
{
    void *(*alloc)(size_t size);
    void *(*realloc)(void *memory, size_t size);
    void (*free)(void *memory);
} book_allocator_t;

/* A converter runs exclusively on the document worker and retains no callbacks
 * or resources when it returns. Its result is independent of the visible page. */
typedef struct book_convert_api
{
    uint32_t abi, size;
    int (*convert)(const char *path, const book_sink_t *sink,
                   const book_allocator_t *allocator, char *error, size_t error_size);
} book_convert_api_t;

/* The private MOBI library exports book_converter. */
#ifdef __cplusplus
}
#endif
#endif
