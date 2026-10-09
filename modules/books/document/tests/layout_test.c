/* Run with a host C compiler and tests/include ahead of the firmware headers. */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../document_view.c"

static unsigned char spool[262144];
static unsigned spool_size;
static bool stream_complete;
static lv_font_t regular = {24, 0}, bold = {24, 0}, heading = {30, 0};
static unsigned prepared_glyphs;
static size_t available_memory = 8u * 1024u * 1024u;

void *epd_app_alloc(size_t n, int region) { (void)region; return malloc(n); }
void *epd_app_realloc(void *p, size_t n, int region) { (void)region; return realloc(p, n); }
void epd_app_free(void *p) { free(p); }
size_t epd_app_psram_available(void) { return available_memory; }
bool epd_app_font_prepare(const lv_font_t *f, uint32_t c, bool adjacent)
{ assert(f && c && adjacent); ++prepared_glyphs; return true; }
void lv_draw_wait_for_finish(void) {}
rt_tick_t rt_tick_get(void) { static rt_tick_t t; return ++t; }
rt_tick_t rt_tick_from_millisecond(int ms) { return ms; }
bool lv_font_get_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *glyph, uint32_t c, uint32_t next)
{ (void)next; glyph->adv_w = c >= 128 ? font->line_height : font->line_height / 2; return true; }
void document_status(document_t *d, document_status_t *s)
{ (void)d; memset(s, 0, sizeof(*s)); s->end = spool_size; s->complete = stream_complete; }
bool document_read(document_t *d, uint32_t off, void *out, size_t n)
{ (void)d; if (off > spool_size || n > spool_size - off) return false; memcpy(out, spool + off, n); return true; }
bool document_record(document_t *d, uint32_t off, document_record_t *r)
{ return document_read(d, off, r, sizeof(*r)) && r->length <= spool_size - off - sizeof(*r); }

static uint32_t emit(unsigned kind, unsigned style, const void *data, unsigned n)
{
    document_record_t r = {kind, n, style, 0};
    uint32_t off = spool_size;
    assert(spool_size + sizeof(r) + n <= sizeof(spool));
    memcpy(spool + spool_size, &r, sizeof(r)); spool_size += sizeof(r);
    if (n) memcpy(spool + spool_size, data, n);
    spool_size += n;
    return off;
}
static void setup(void)
{
    free(view.toc); memset(&view, 0, sizeof(view));
    view.fonts[0] = &regular; view.fonts[1] = &bold; view.fonts[2] = &heading;
    view.view.margin = 32; view.view.line_space = 4;
    spool_size = 0; stream_complete = false;
}
static unsigned paginate(char *output, size_t capacity, document_cursor_t *positions, unsigned *image_count)
{
    view_page_t *page = calloc(1, sizeof(*page));
    document_cursor_t start = {0, 0};
    unsigned pages = 0, bytes = 0;
    do
    {
        page_reset(page, start);
        unsigned watchdog = 0;
        while (!page->done)
        {
            assert(page_step(page, true) == 1);
            assert(++watchdog < 100000);
        }
        if (positions) positions[pages] = start;
        for (unsigned i = 0; i < page->count; ++i)
        {
            view_run_t *run = &page->runs[i];
            assert(run->x + run->width <= width_available());
            assert(run->y + run->height <= VIEW_HEIGHT);
            if (run->image) { ++*image_count; continue; }
            assert(bytes + run->bytes < capacity);
            memcpy(output + bytes, page->text + run->text, run->bytes); bytes += run->bytes;
        }
        assert(page->eof || cursor_before(start, page->cursor));
        start = page->cursor; ++pages;
        assert(pages < 128);
    } while (!page->eof);
    output[bytes] = 0;
    free(page);
    return pages;
}
static void test_streaming_and_styles(void)
{
    setup();
    view_page_t *p = calloc(1, sizeof(*p));
    page_reset(p, (document_cursor_t){0, 0});
    assert(page_step(p, false) == 0);
    emit(DOCUMENT_TEXT, BOOK_STYLE_BOLD, "hello", 5);
    for (int i = 0; i < 6; ++i) assert(page_step(p, false) == 1);
    assert(page_step(p, false) == 0 && !p->done);
    emit(DOCUMENT_TEXT, BOOK_STYLE_ITALIC, " 世界", 7);
    stream_complete = true;
    while (!p->done) assert(page_step(p, false) == 1);
    assert(p->count == 2 && p->runs[0].style == BOOK_STYLE_BOLD && p->runs[1].style == BOOK_STYLE_ITALIC);
    assert(!strcmp(p->text + p->runs[0].text, "hello"));
    assert(!strcmp(p->text + p->runs[1].text, " 世界"));
    free(p);
}
static void test_pages_and_reflow(void)
{
    setup();
    char *expected = calloc(1, 60000), *actual = calloc(1, 60000);
    unsigned n = 0;
    for (unsigned i = 0; i < 90; ++i)
    {
        char text[512]; unsigned len = 0;
        for (unsigned j = 0; j < 50; ++j) { memcpy(text + len, "中ab ", 6); len += 6; }
        emit(DOCUMENT_TEXT, i % 3 == 0 ? BOOK_STYLE_BOLD : 0, text, len);
        memcpy(expected + n, text, len); n += len;
    }
    stream_complete = true;
    document_cursor_t starts[128]; unsigned images = 0;
    unsigned first_pages = paginate(actual, 60000, starts, &images);
    assert(first_pages > 5 && !strcmp(expected, actual));
    document_cursor_t anchor = starts[3];
    regular.line_height = bold.line_height = 36;
    unsigned second_pages = paginate(actual, 60000, starts, &images);
    assert(second_pages > first_pages && !strcmp(expected, actual));
    unsigned restored = 0;
    for (unsigned i = 0; i < second_pages; ++i) if (!cursor_before(anchor, starts[i])) restored = i;
    assert(!cursor_before(anchor, starts[restored]));
    assert(restored + 1 == second_pages || cursor_before(anchor, starts[restored + 1]));
    regular.line_height = bold.line_height = 24;
    free(actual); free(expected);
}
static void test_toc_definition_and_image(void)
{
    setup();
    const char toc[] = "chapter#one\0First chapter\0";
    const char anchor[] = "chapter#one\0\0";
    emit(DOCUMENT_ANCHOR, 1, toc, sizeof(toc) - 1);
    emit(DOCUMENT_TEXT, 0, "preface", 7);
    emit(DOCUMENT_PARAGRAPH, 0, NULL, 0);
    unsigned target = emit(DOCUMENT_ANCHOR, 0, anchor, sizeof(anchor) - 1);
    emit(DOCUMENT_TEXT, BOOK_STYLE_HEADING, "Chapter", 7);
    document_image_t image = {620, 1040};
    emit(DOCUMENT_IMAGE, 0, &image, sizeof(image));
    stream_complete = true;
    char output[256]; unsigned images = 0;
    unsigned pages = paginate(output, sizeof(output), NULL, &images);
    assert(pages >= 2 && images == 1 && !strcmp(output, "prefaceChapter"));
    assert(view.toc_count == 1 && document_view_toc_seek(0));
    for (unsigned i = 0; view.link_target[0] && i < 20; ++i) link_process();
    assert(!view.link_target[0] && view.restoring && view.anchor.record == target);
}
static void test_neighbour_prepare(void)
{
    setup();
    char text[1000]; memset(text, 'z', sizeof(text));
    for (unsigned i = 0; i < 12; ++i) emit(DOCUMENT_TEXT, 0, text, sizeof(text));
    stream_complete = true;
    document_cursor_t pages[128]; char output[16000]; unsigned images = 0;
    unsigned count = paginate(output, sizeof(output), pages, &images);
    pages[count] = (document_cursor_t){spool_size, 0};
    assert(count > 3);
    view.document = (document_t *)1; view.pages = pages; view.count = count;
    view.layout = view.display_layout = 1; view.view.ready = true; view.view.page = 2; view.wanted = 1;
    uint32_t revision = view.view.revision;
    for (unsigned i = 0; i < 6000; ++i)
    {
        document_view_prepare();
        if (view.neighbours[0].valid && view.neighbours[1].valid &&
            view.neighbours[0].warm_run == view.neighbours[0].page->count &&
            view.neighbours[1].warm_run == view.neighbours[1].page->count) break;
    }
    assert(view.neighbours[0].valid && view.neighbours[0].number == 2);
    assert(view.neighbours[1].valid && view.neighbours[1].number == 0);
    assert(prepared_glyphs && view.view.revision == revision && view.view.page == 2);
    available_memory = 0; document_view_prepare(); available_memory = 8u * 1024u * 1024u;
    assert(view.neighbours[0].valid && view.neighbours[1].valid);
    for (unsigned i = 0; i < 2; ++i) { free(view.neighbours[i].page); view.neighbours[i].page = NULL; }
    view.pages = NULL; view.document = NULL;
}
static void test_thin_image_scaling(void)
{
    setup();
    view.view.margin = 44;
    document_image_t image = {620, 1};
    emit(DOCUMENT_IMAGE, 0, &image, sizeof(image));
    stream_complete = true;
    view_page_t *page = calloc(1, sizeof(*page));
    assert(page);
    assert(page_step(page, false) == 1);
    assert(page->count == 1 && page->runs[0].width == 596 && page->runs[0].height == 1);
    free(page);
}

int main(void)
{
    test_streaming_and_styles(); test_pages_and_reflow(); test_toc_definition_and_image();
    test_neighbour_prepare();
    test_thin_image_scaling();
    free(view.toc);
    puts("PASS: streaming, UTF-8, styles, pagination, reflow anchors, deferred TOC, images, adjacent pages/glyphs without redraw or free-space gate");
    return 0;
}
