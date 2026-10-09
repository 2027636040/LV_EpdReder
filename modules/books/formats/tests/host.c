/* SPDX-License-Identifier: Apache-2.0 */
#include "formats.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static size_t calls, live, peak, bytes, fail_at, events, cancel_at;
static int image_result;
typedef union allocation
{
    size_t size;
    long double align;
    void *pointer;
} allocation;
static void *allocate(size_t n)
{
    allocation *p;
    ++calls;
    if (fail_at && calls == fail_at)
        return NULL;
    p = malloc(sizeof(*p) + n);
    if (!p)
        return NULL;
    p->size = n;
    bytes += n;
    if (bytes > peak)
        peak = bytes;
    ++live;
    return p + 1;
}
static void release(void *p)
{
    allocation *a;
    if (!p)
        return;
    a = (allocation *)p - 1;
    bytes -= a->size;
    --live;
    free(a);
}
static void *resize(void *p, size_t n)
{
    void *q;
    size_t k;
    if (!p)
        return allocate(n);
    q = allocate(n);
    if (!q)
        return NULL;
    k = ((allocation *)p - 1)->size;
    memcpy(q, p, k < n ? k : n);
    release(p);
    return q;
}
static bool cancelled(void *u)
{
    (void)u;
    return cancel_at && events >= cancel_at;
}
static int text(void *u, const char *s, size_t n, unsigned style)
{
    (void)u;
    ++events;
    printf("TEXT %u ", style);
    fwrite(s, 1, n, stdout);
    putchar('\n');
    return 0;
}
static int para(void *u)
{
    (void)u;
    ++events;
    puts("PARA");
    return 0;
}
static int anchor(void *u, const char *id, const char *title, unsigned level)
{
    (void)u;
    ++events;
    printf("ANCHOR %u %s | %s\n", level, id, title ? title : "");
    return 0;
}
static int link(void *u, const char *target, bool begin)
{
    (void)u;
    ++events;
    printf("LINK %d %s\n", begin, target);
    return 0;
}
static int metadata(void *u, const char *title, const char *author)
{
    (void)u;
    ++events;
    printf("META %s | %s\n", title, author);
    return 0;
}
static int picture(void *u, const char *id, book_stream_t *s)
{
    unsigned char b[7];
    size_t n = 0;
    unsigned long hash = 0;
    int r;
    size_t i;
    (void)u;
    ++events;
    if (image_result)
    {
        r = s->read(s->user, b, 1);
        if (r < 0)
            return r;
        return image_result == 1 ? BOOK_OK : image_result;
    }
    while ((r = s->read(s->user, b, sizeof(b))) > 0)
    {
        n += (size_t)r;
        for (i = 0; i < (size_t)r; ++i)
            hash = hash * 33 + b[i];
    }
    printf("IMAGE %s %lu %lu\n", id, (unsigned long)n, hash);
    return r;
}
int main(int argc, char **argv)
{
    static const char *unsupported[] = {".pdf", ".doc", ".docx", ".fb2", ".azw3"};
    for (unsigned n = 0; n < sizeof(unsupported) / sizeof(unsupported[0]); ++n)
        assert(book_format_detect(unsupported[n], false) == BOOK_FORMAT_UNKNOWN);
    assert(book_format_detect("book.TXT", false) == BOOK_FORMAT_TXT);
    assert(book_format_detect("book.MD", false) == BOOK_FORMAT_MARKDOWN);
    assert(book_format_detect("book.markdown", false) == BOOK_FORMAT_MARKDOWN);
    assert(book_format_detect("book.EPUB", false) == BOOK_FORMAT_EPUB);
    assert(book_format_detect("book.MOBI", false) == BOOK_FORMAT_MOBI);
    book_allocator_t a = {allocate, resize, release};
    book_sink_t sink = {0};
    char error[256];
    int r, repeat = 1, i;
    if (argc < 2)
        return 2;
    if (argc > 2)
        fail_at = strtoul(argv[2], NULL, 10);
    if (argc > 3)
        cancel_at = strtoul(argv[3], NULL, 10);
    if (argc > 4)
        image_result = atoi(argv[4]);
    if (argc > 5)
        repeat = atoi(argv[5]);
    sink.text = text;
    sink.paragraph = para;
    sink.anchor = anchor;
    sink.link = link;
    sink.image = picture;
    sink.metadata = metadata;
    sink.cancelled = cancelled;
    sink.markup = book_markup_convert;
    r = BOOK_ERROR;
    for (i = 0; i < repeat; ++i)
    {
        r = book_format_convert(argv[1], &sink, &a, error, sizeof(error));
        printf("RESULT %d live=%lu peak=%lu allocations=%lu error=%s\n", r, (unsigned long)live,
               (unsigned long)peak, (unsigned long)calls, error);
        if (live)
            return 3;
        fail_at = cancel_at = 0;
        image_result = 0;
    }
    return live ? 3 : r == 0 ? 0 : 1;
}
