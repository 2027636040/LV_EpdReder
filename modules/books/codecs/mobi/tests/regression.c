/* Host regression: allocator failure, cancellation, media change and cleanup. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#undef open
#undef read
#undef lseek
#undef close
#define _open open
#define _read read
#define _lseek lseek
#define _close close
#endif
#ifdef TEST_COMMON_MARKUP
#include "formats.h"
#endif
#include "book_codec.h"
#include "storage.h"
#include "dfs_file.h"
extern const book_convert_api_t book_converter;
static size_t calls, fail_at, live, bytes_live, peak, text_bytes, markup_bytes, anchors, images, paragraphs, styles;
static size_t polls, cancel_at, change_at, callbacks, error_at;
static unsigned revision = 1;
static int locked, files;
typedef union { size_t size; unsigned char padding[16]; } host_allocation_t;
int test_open(const char *path, int flags)
{ int fd; assert(locked); fd = _open(path, flags); if (fd >= 0) ++files; return fd; }
int test_read(int fd, void *buffer, unsigned size)
{ assert(locked); return _read(fd, buffer, size); }
long test_lseek(int fd, long offset, int origin)
{ assert(locked); return _lseek(fd, offset, origin); }
int test_close(int fd) { assert(locked); --files; return _close(fd); }
void storage_lock(void) { assert(!locked); locked = 1; }
void storage_unlock(void) { assert(locked); locked = 0; }
bool storage_changing(void) { return false; }
uint32_t storage_revision(void) { return revision; }
int dfs_file_open(struct dfs_fd *file, const char *path, int flags)
{
    assert(locked);
    file->fd = _open(path, flags);
    if (file->fd < 0) return -1;
    ++files; file->size = _lseek(file->fd, 0, SEEK_END); _lseek(file->fd, 0, SEEK_SET); return 0;
}
int dfs_file_read(struct dfs_fd *file, void *buffer, size_t length)
{ assert(locked); return _read(file->fd, buffer, (unsigned)length); }
int dfs_file_lseek(struct dfs_fd *file, long offset)
{ assert(locked); return _lseek(file->fd, offset, SEEK_SET); }
int dfs_file_close(struct dfs_fd *file)
{ assert(locked); --files; return _close(file->fd); }
static void *allocate(size_t size)
{
    host_allocation_t *p;
    assert(!locked);
    if (++calls == fail_at) return NULL;
    p = malloc(sizeof(*p) + size);
    assert(p); p->size = size;
    ++live; bytes_live += size; if (bytes_live > peak) peak = bytes_live;
    return p + 1;
}
static void release(void *memory)
{
    host_allocation_t *p;
    if (!memory) return;
    assert(!locked && live);
    p = (host_allocation_t *)memory - 1; --live; bytes_live -= p->size; free(p);
}
static void *resize(void *memory, size_t size)
{
    void *p = allocate(size);
    if (p && memory) { size_t old = ((host_allocation_t *)memory - 1)->size; memcpy(p, memory, old < size ? old : size); release(memory); }
    return p;
}
static bool cancelled(void *user)
{
    (void)user; assert(!locked);
    ++polls;
    if (polls == change_at) ++revision;
    return cancel_at && polls >= cancel_at;
}
static int event(void) { assert(!locked); return ++callbacks == error_at ? BOOK_IO_ERROR : BOOK_OK; }
static int text(void *user, const char *utf8, size_t length, unsigned style)
{ (void)user; (void)utf8; text_bytes += length; styles |= style; return event(); }
static int paragraph(void *user) { (void)user; ++paragraphs; return event(); }
static int anchor(void *user, const char *id, const char *title, unsigned level)
{ (void)user; (void)title; (void)level; assert(id && *id); ++anchors; return event(); }
static int metadata(void *user, const char *title, const char *author)
{ (void)user; (void)title; (void)author; return event(); }
static int emit_link(void *user, const char *target, bool begin)
{ (void)user; (void)target; (void)begin; return event(); }
static int image(void *user, const char *id, book_stream_t *stream)
{
    char buffer[211]; int n, result;
    (void)user; (void)id; ++images;
    while ((n = stream->read(stream->user, buffer, sizeof(buffer))) > 0) {
        result = event(); if (result) return result;
    }
    return n < 0 ? n : event();
}
static const char *attr(char *tag, const char *name)
{
    char key[40], *p, *end;
    snprintf(key, sizeof(key), "%s=\"", name);
    p = strstr(tag, key);
    if (!p) return NULL;
    p += strlen(key); end = strchr(p, '"'); if (!end) return NULL; *end = 0; return p;
}
static int markup(const book_sink_t *sink, book_stream_t *input,
                  const char *base, const book_resources_t *resources)
{
    char buffer[251], tag[8192], copy[8192];
    size_t t = 0;
    int n, inside = 0, result = 0;
    while ((n = input->read(input->user, buffer, sizeof(buffer))) > 0) {
        int i; markup_bytes += n;
        for (i = 0; i < n; ++i) {
            char ch = buffer[i];
            if (ch == '<') { inside = 1; t = 0; }
            if (inside) {
                if (t + 1 < sizeof(tag)) tag[t++] = ch;
                if (ch == '>') {
                    const char *value;
                    tag[t] = 0; inside = 0;
                    strcpy(copy, tag); value = attr(copy, "id");
                    if (value && sink->anchor) result = sink->anchor(sink->user, value, NULL, 0);
                    if (result) return result;
                    strcpy(copy, tag); value = attr(copy, "href");
                    if (value && sink->link) result = sink->link(sink->user, value, true);
                    if (result) return result;
                    strcpy(copy, tag); value = attr(copy, "src");
                    if (value && !strncmp(tag, "<img", 4)) {
                        book_stream_t stream = {0};
                        result = resources->open(resources->user, base, value, &stream);
                        if (result == BOOK_OK) {
                            result = sink->image(sink->user, value, &stream);
                            if (stream.close) stream.close(stream.user);
                        } else if (result == BOOK_UNSUPPORTED) result = BOOK_OK;
                        if (result) return result;
                    }
                }
            } else { result = sink->text(sink->user, &ch, 1, 0); if (result) return result; }
        }
    }
    return n < 0 ? n : 0;
}
static int run(const char *path)
{
    char error[256]; int result;
    book_allocator_t allocator = { allocate, resize, release };
    book_sink_t sink = { NULL, cancelled, text, paragraph, anchor, image, metadata, emit_link, markup };
    calls = polls = callbacks = peak = text_bytes = markup_bytes = anchors = images = paragraphs = styles = 0;
#ifdef TEST_COMMON_MARKUP
    sink.markup = book_markup_convert;
    result = book_markup_begin(&allocator);
    if (result == BOOK_OK) result = book_converter.convert(path, &sink, &allocator, error, sizeof(error));
    book_markup_end();
#else
    result = book_converter.convert(path, &sink, &allocator, error, sizeof(error));
#endif
    assert(live == 0 && bytes_live == 0 && files == 0 && !locked);
    return result;
}
int main(int argc, char **argv)
{
    size_t baseline, checks, i;
    int result;
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (argc < 2) return 2;
    result = run(argv[1]); baseline = calls;
    printf("baseline result=%d allocations=%zu peak=%zu text=%zu markup=%zu anchors=%zu paragraphs=%zu images=%zu styles=%zu\n",
           result, calls, peak, text_bytes, markup_bytes, anchors, paragraphs, images, styles);
    if (argc > 2 && !strcmp(argv[2], "reject")) return result == BOOK_OK ? 6 : 0;
    if (result != BOOK_OK) return 1;
    assert(text_bytes > 0);
    if (argc > 3) {
        fail_at = strtoul(argv[3], NULL, 10);
        fprintf(stderr, "single allocation failure point %zu\n", fail_at);
        result = run(argv[1]);
        printf("single failure result=%d\n", result);
        return result == BOOK_NO_MEMORY ? 0 : 3;
    }
    checks = argc > 2 ? strtoul(argv[2], NULL, 10) : baseline;
    if (checks > baseline) checks = baseline;
    for (i = 1; i <= checks; ++i) {
#ifdef TEST_COMMON_MARKUP
        fprintf(stderr, "allocation failure point %zu\n", i);
#endif
        fail_at = i; result = run(argv[1]);
        if (result != BOOK_NO_MEMORY) { printf("allocation failure %zu returned %d\n", i, result); return 3; }
    }
    fail_at = 0;
    for (i = 1; i <= 80; ++i) {
        cancel_at = i; result = run(argv[1]);
        if (result != BOOK_CANCELLED) { printf("cancel %zu returned %d\n", i, result); return 4; }
    }
    cancel_at = 0;
    for (i = 1; i <= 80; ++i) {
        change_at = i; result = run(argv[1]);
        if (result != BOOK_IO_ERROR) { printf("media change %zu returned %d\n", i, result); return 5; }
    }
    change_at = 0; error_at = 2; result = run(argv[1]); assert(result == BOOK_IO_ERROR);
    error_at = 0; result = run(argv[1]); assert(result == BOOK_OK);
    printf("PASS %zu allocation failures, 80 cancellation points, 80 media changes, callback failure, repeat-open; no outstanding allocations or DFS handles\n", checks);
    return 0;
}
