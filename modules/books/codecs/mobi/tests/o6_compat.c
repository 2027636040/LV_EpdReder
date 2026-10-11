/* Compare raw markup and the complete common-parser callback byte streams. */
#define main regression_main
#include "regression.c"
#undef main

extern const book_convert_api_t baseline_book_converter;
typedef struct { unsigned char *data; size_t size, capacity; } bytes_t;
typedef struct { bytes_t events, markup; } capture_t;
static capture_t *capture;

static void append(bytes_t *bytes, const void *data, size_t size)
{
    if (bytes->capacity - bytes->size < size) {
        size_t capacity = (bytes->size + size) * 2 + 1024;
        void *p = realloc(bytes->data, capacity);
        assert(p);
        bytes->data = p;
        bytes->capacity = capacity;
    }
    if (size) memcpy(bytes->data + bytes->size, data, size);
    bytes->size += size;
}
static void number(uint64_t value)
{
    unsigned char data[8];
    unsigned i;
    for (i = 0; i < 8; ++i) data[i] = (unsigned char)(value >> (i * 8));
    append(&capture->events, data, sizeof(data));
}
static void string(const char *text)
{
    number(text ? strlen(text) + 1 : 0);
    if (text) append(&capture->events, text, strlen(text) + 1);
}
static int capture_text(void *user, const char *text, size_t size, unsigned style)
{
    (void)user;
    number('T'); number(style); number(size); append(&capture->events, text, size);
    return BOOK_OK;
}
static int capture_paragraph(void *user)
{ (void)user; number('P'); return BOOK_OK; }
static int capture_anchor(void *user, const char *id, const char *title, unsigned level)
{ (void)user; number('A'); string(id); string(title); number(level); return BOOK_OK; }
static int capture_metadata(void *user, const char *title, const char *author)
{ (void)user; number('M'); string(title); string(author); return BOOK_OK; }
static int capture_link(void *user, const char *target, bool begin)
{ (void)user; number('L'); string(target); number(begin); return BOOK_OK; }
static int capture_image(void *user, const char *id, book_stream_t *stream)
{
    char buffer[211];
    int n;
    (void)user;
    number('I'); string(id);
    while ((n = stream->read(stream->user, buffer, sizeof(buffer))) > 0) {
        number((unsigned)n); append(&capture->events, buffer, n);
    }
    number(0);
    return n < 0 ? n : BOOK_OK;
}
static int capture_read(void *user, void *buffer, size_t size)
{
    book_stream_t *input = user;
    int n = input->read(input->user, buffer, size);
    if (n > 0) append(&capture->markup, buffer, (size_t)n);
    return n;
}
static int capture_markup(const book_sink_t *sink, book_stream_t *input,
                          const char *base, const book_resources_t *resources)
{
    book_stream_t tee = { input, input->size, capture_read, NULL, NULL };
    number('B'); string(base);
#ifdef TEST_COMMON_MARKUP
    return book_markup_convert(sink, &tee, base, resources);
#else
    return markup(sink, &tee, base, resources);
#endif
}
static int capture_run(const book_convert_api_t *api, const char *path, capture_t *out)
{
    char error[256];
    int result;
    book_allocator_t allocator = { allocate, resize, release };
    book_sink_t sink = { NULL, cancelled, capture_text, capture_paragraph,
        capture_anchor, capture_image, capture_metadata, capture_link, capture_markup };
    capture = out;
    calls = polls = callbacks = peak = 0;
    fail_at = cancel_at = change_at = error_at = 0;
#ifdef TEST_COMMON_MARKUP
    assert(book_markup_begin(&allocator) == BOOK_OK);
#endif
    result = api->convert(path, &sink, &allocator, error, sizeof(error));
#ifdef TEST_COMMON_MARKUP
    book_markup_end();
#endif
    assert(live == 0 && bytes_live == 0 && files == 0 && !locked);
    return result;
}
static void equal_bytes(const bytes_t *a, const bytes_t *b)
{
    size_t i;
    if (a->size != b->size) {
        fprintf(stderr, "byte length mismatch: %zu != %zu\n", a->size, b->size);
        abort();
    }
    for (i = 0; i < a->size; ++i) if (a->data[i] != b->data[i]) {
        fprintf(stderr, "byte mismatch at %zu: %u != %u\n", i, a->data[i], b->data[i]);
        abort();
    }
}
int main(int argc, char **argv)
{
    int i;
    assert(argc > 1);
    for (i = 1; i < argc; ++i) {
        capture_t old = {0}, current = {0};
        int old_result = capture_run(&baseline_book_converter, argv[i], &old);
        int new_result = capture_run(&book_converter, argv[i], &current);
        assert(old_result == new_result);
        equal_bytes(&old.markup, &current.markup);
        equal_bytes(&old.events, &current.events);
        printf("PASS byte compatibility: %s result=%d markup=%zu events=%zu\n",
               argv[i], new_result, current.markup.size, current.events.size);
        free(old.markup.data); free(old.events.data);
        free(current.markup.data); free(current.events.data);
    }
    return 0;
}
