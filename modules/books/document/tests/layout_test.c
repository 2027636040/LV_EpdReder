/* Run with a host C compiler and tests/include ahead of the firmware headers. */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define rt_kprintf printf
#include "../../../common/reader/reader_view.c"
#include "../document_view.c"

static unsigned char spool[4u * 1024u * 1024u];
static unsigned spool_size;
static bool stream_complete;
static lv_font_t regular = {24, 0}, bold = {24, 0}, heading = {30, 0};
static unsigned prepared_glyphs, glyph_queries, status_queries, progress_queries, read_queries;
static unsigned reader_reads[4];
static unsigned image_info_calls, image_info_invalidations;
static int allocation_fail_after = -1;
static size_t available_memory = 8u * 1024u * 1024u;

void *epd_app_alloc(size_t n, int region)
{ (void)region; if (allocation_fail_after == 0) return NULL; if (allocation_fail_after > 0) --allocation_fail_after; return malloc(n); }
void *epd_app_realloc(void *p, size_t n, int region) { (void)region; return realloc(p, n); }
void epd_app_free(void *p) { free(p); }
size_t epd_app_psram_available(void) { return available_memory; }
bool epd_app_font_prepare(const lv_font_t *f, uint32_t c, bool adjacent)
{ assert(f && c && adjacent); ++prepared_glyphs; return true; }
void lv_draw_wait_for_finish(void) {}
rt_tick_t rt_tick_get(void) { static rt_tick_t t; return ++t; }
rt_tick_t rt_tick_from_millisecond(int ms) { return ms; }
bool lv_font_get_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *glyph, uint32_t c, uint32_t next)
{ ++glyph_queries; (void)next; glyph->adv_w = c >= 128 ? font->line_height : font->line_height / 2; return true; }
void document_status(document_t *d, document_status_t *s)
{ ++status_queries; (void)d; memset(s, 0, sizeof(*s)); s->end = spool_size; s->complete = stream_complete; }
bool document_read(document_t *d, uint32_t off, void *out, size_t n)
{ (void)d; ++read_queries; if (off > spool_size || n > spool_size - off) return false; memcpy(out, spool + off, n); return true; }
bool document_record(document_t *d, uint32_t off, document_record_t *r)
{ return document_read(d, off, r, sizeof(*r)) && r->length <= spool_size - off - sizeof(*r); }

void document_progress(document_t *d, document_progress_t *p)
{ (void)d; ++progress_queries; *p = (document_progress_t){spool_size, 1, stream_complete, 0}; }
bool document_reader_read(document_reader_t *r, uint32_t off, void *out, size_t n)
{ if ((uintptr_t)r < 4) ++reader_reads[(uintptr_t)r]; return document_read(NULL, off, out, n); }
bool document_reader_record(document_reader_t *r, uint32_t off, document_record_t *record)
{ if ((uintptr_t)r < 4) ++reader_reads[(uintptr_t)r]; return document_record(NULL, off, record); }
bool document_reader_image_info(document_reader_t *reader, uint32_t offset, document_image_t *image)
{
    ++image_info_calls;
    document_record_t record;
    if (image && document_reader_record(reader, offset, &record) && record.kind == DOCUMENT_IMAGE &&
        record.length >= sizeof(*image) &&
        document_reader_read(reader, offset + sizeof(record), image, sizeof(*image)) &&
        image->width && image->height && image->width <= DOCUMENT_IMAGE_WIDTH &&
        image->height <= DOCUMENT_IMAGE_HEIGHT &&
        record.length == sizeof(*image) + (image->width * image->height + 1) / 2) return true;
    ++image_info_invalidations;
    return false;
}
#ifndef VIEW_INTEGRATION_TEST
void document_image_cancel(document_image_request_t *r) { assert(!r); }
#endif
static int test_step(view_page_t *page, bool indexing)
{
    document_progress_t progress;
    document_progress(NULL, &progress);
    return page_step(page, indexing, NULL, &progress);
}
static void legacy_line_finish(view_page_t *p)
{
    unsigned delta = p->line_style & BOOK_STYLE_CENTER ? (width_available() - p->x) / 2 :
                     p->line_style & BOOK_STYLE_RIGHT ? width_available() - p->x : 0;
    for (unsigned i = p->line_start; i < p->count; ++i) p->runs[i].x += delta;
    p->y += (p->line_height ? p->line_height : view.fonts[0]->line_height) + view.view.line_space;
    p->x = 0; p->line_height = 0; p->line_start = p->count; p->line_style = 0;
}
static unsigned legacy_utf8_letter(const char *p, unsigned n, uint32_t *letter)
{
    unsigned char c = (unsigned char)*p;
    unsigned length = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    if (length > n || c < 0xc2 || c > 0xf4)
    { *letter = c < 0x80 ? c : '?'; return 1; }
    uint32_t value = c & ((1u << (7 - length)) - 1);
    for (unsigned i = 1; i < length; ++i)
    {
        if (((unsigned char)p[i] & 0xc0) != 0x80) { *letter = '?'; return 1; }
        value = (value << 6) | ((unsigned char)p[i] & 0x3f);
    }
    *letter = value;
    return length;
}
static bool legacy_append_letter(view_page_t *p, const char *text, unsigned bytes, uint32_t letter)
{
    unsigned style = p->record.style;
    const lv_font_t *font = font_for(style);
    lv_font_glyph_dsc_t glyph;
    memset(&glyph, 0, sizeof(glyph));
    lv_font_get_glyph_dsc(font, &glyph, letter == '\t' ? ' ' : letter, 0);
    unsigned width = glyph.adv_w * (letter == '\t' ? 4 : 1);
    if (width > width_available()) width = width_available();
    if (p->x && p->x + width > width_available()) legacy_line_finish(p);
    if (p->y + font->line_height > VIEW_HEIGHT) { p->done = true; return false; }
    if (p->bytes + (letter == '\t' ? 4 : bytes) + 2 >= VIEW_TEXT || p->count >= VIEW_RUNS - 1)
    { if (p->x) legacy_line_finish(p); p->done = true; return false; }
    view_run_t *run = p->count ? &p->runs[p->count - 1] : NULL;
    if (!run || run->image || run->y != p->y || run->style != style || run->link != p->record.link)
    {
        run = &p->runs[p->count++];
        memset(run, 0, sizeof(*run));
        run->x = p->x; run->y = p->y; run->text = p->bytes;
        run->style = style; run->link = p->record.link;
    }
    else if (p->bytes && !p->text[p->bytes - 1]) --p->bytes;
    if (letter == '\t')
    {
        memcpy(p->text + p->bytes, "    ", 4); p->bytes += 4; run->bytes += 4;
    }
    else { memcpy(p->text + p->bytes, text, bytes); p->bytes += bytes; run->bytes += bytes; }
    p->text[p->bytes++] = 0;
    run->width += width; run->height = font->line_height;
    p->x += width;
    if (font->line_height > p->line_height) p->line_height = font->line_height;
    p->line_style |= style;
    return true;
}

/* A step consumes one UTF-8 character or one structural event. */
static int legacy_page_step(view_page_t *p, bool indexing)
{
    document_status_t status;
    document_status(view.document, &status);
    if (p->done) return 1;
    if (!p->has_record || p->loaded != p->cursor.record)
    {
        if (p->cursor.record == status.end)
        {
            if (!status.complete) return 0;
            if (p->x) legacy_line_finish(p);
            p->done = p->eof = true;
            return 1;
        }
        if (!document_record(view.document, p->cursor.record, &p->record)) return -1;
        p->loaded = p->cursor.record; p->has_record = true;
        if (p->record.kind != DOCUMENT_IMAGE)
        {
            if (p->record.length > DOCUMENT_TEXT_LIMIT ||
                !document_read(view.document, p->cursor.record + sizeof(document_record_t), p->data, p->record.length)) return -1;
            p->data[p->record.length] = 0;
        }
    }
    switch (p->record.kind)
    {
    case DOCUMENT_TEXT:
        if (p->cursor.byte < p->record.length)
        {
            uint32_t letter;
            unsigned n = legacy_utf8_letter(p->data + p->cursor.byte, p->record.length - p->cursor.byte, &letter);
            if (letter == '\r') { p->cursor.byte += n; return 1; }
            if (letter == '\n') { legacy_line_finish(p); p->cursor.byte += n; return 1; }
            if (!(p->record.style & BOOK_STYLE_PREFORMAT) && p->x && letter < 128 &&
                isalnum((unsigned char)letter) && (!p->cursor.byte ||
                isspace((unsigned char)p->data[p->cursor.byte - 1])))
            {
                unsigned word_width = 0;
                const lv_font_t *font = font_for(p->record.style);
                for (unsigned at = p->cursor.byte; at < p->record.length; ++at)
                {
                    unsigned char c = p->data[at];
                    if (c >= 128 || isspace(c)) break;
                    lv_font_glyph_dsc_t glyph;
                    memset(&glyph, 0, sizeof(glyph));
                    lv_font_get_glyph_dsc(font, &glyph, c, 0);
                    word_width += glyph.adv_w;
                    if (word_width > width_available()) break;
                }
                if (word_width <= width_available() && p->x + word_width > width_available()) legacy_line_finish(p);
            }
            if (!legacy_append_letter(p, p->data + p->cursor.byte, n, letter)) return 1;
            p->cursor.byte += n;
            return 1;
        }
        break;
    case DOCUMENT_PARAGRAPH:
        if (p->x) legacy_line_finish(p);
        p->y += view.fonts[0]->line_height / 3;
        break;
    case DOCUMENT_ANCHOR:
        if (indexing && p->cursor.record >= view.toc_scanned)
        {
            size_t id = strlen(p->data);
            if (id + 1 < p->record.length && p->data[id + 1] && !add_toc(p->cursor.record, p->record.style)) return -1;
            view.toc_scanned = p->cursor.record + sizeof(document_record_t) + p->record.length;
        }
        break;
    case DOCUMENT_IMAGE:
    {
        document_image_t image;
        if (!document_read(view.document, p->cursor.record + sizeof(document_record_t), &image, sizeof(image)) ||
            !image.width || !image.height || image.width > DOCUMENT_IMAGE_WIDTH || image.height > DOCUMENT_IMAGE_HEIGHT) return -1;
        if (p->x) legacy_line_finish(p);
        unsigned width = image.width, height = image.height;
        if (width > width_available())
        {
            height = height * width_available() / width;
            if (!height) height = 1;
            width = width_available();
        }
        if (height > VIEW_HEIGHT)
        {
            width = width * VIEW_HEIGHT / height;
            if (!width) width = 1;
            height = VIEW_HEIGHT;
        }
        if (p->y + height > VIEW_HEIGHT && p->count) { p->done = true; return 1; }
        if (p->count == VIEW_RUNS) { p->done = true; return 1; }
        view_run_t *run = &p->runs[p->count++];
        memset(run, 0, sizeof(*run));
        run->image = p->cursor.record + 1; run->link = p->record.link;
        run->width = width; run->height = height;
        run->x = (width_available() - width) / 2; run->y = p->y;
        p->y += height + view.view.line_space;
        p->line_start = p->count;
        break;
    }
    default: break;
    }
    p->cursor.record += sizeof(document_record_t) + p->record.length;
    p->cursor.byte = 0; p->has_record = false;
    return 1;
}


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
static uint32_t emit_image(unsigned width, unsigned height)
{
    assert(width && height && width <= DOCUMENT_IMAGE_WIDTH && height <= DOCUMENT_IMAGE_HEIGHT);
    document_image_t image = {width, height};
    size_t bytes = sizeof(image) + (width * height + 1) / 2;
    unsigned char *data = calloc(1, bytes); assert(data);
    memcpy(data, &image, sizeof(image));
    uint32_t offset = emit(DOCUMENT_IMAGE, 0, data, bytes);
    free(data); return offset;
}
#define INVALID_IMAGE_CASES 7u
static uint32_t emit_invalid_image(unsigned scenario)
{
    document_image_t image = {3, 3};
    unsigned char data[sizeof(image) + 6] = {0};
    unsigned length = sizeof(image);
    switch (scenario)
    {
    case 0: length = sizeof(image) - 1; break;
    case 1: image.width = 0; break;
    case 2: image.height = 0; break;
    case 3: image.width = DOCUMENT_IMAGE_WIDTH + 1; break;
    case 4: image.height = DOCUMENT_IMAGE_HEIGHT + 1; break;
    case 5: length += 4; break; /* Nine pixels require five packed bytes. */
    case 6: length += 6; break;
    default: assert(!"unknown image corruption case");
    }
    memcpy(data, &image, sizeof(image));
    return emit(DOCUMENT_IMAGE, 0, data, length);
}
static void setup(void)
{
    free(view.toc); memset(&view, 0, sizeof(view));
    view.fonts[0] = &regular; view.fonts[1] = &bold; view.fonts[2] = &heading;
    view.view.margin = 32; view.view.line_space = 4;
    spool_size = 0; stream_complete = false;
    image_info_calls = image_info_invalidations = 0;
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
            assert(test_step(page, true) == 1);
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
    assert(test_step(p, false) == 0);
    emit(DOCUMENT_TEXT, BOOK_STYLE_BOLD, "hello", 5);
    assert(test_step(p, false) == 1);
    assert(test_step(p, false) == 0 && !p->done);
    emit(DOCUMENT_TEXT, BOOK_STYLE_ITALIC, " 世界", 7);
    stream_complete = true;
    while (!p->done) assert(test_step(p, false) == 1);
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
    emit_image(620, 1040);
    stream_complete = true;
    char output[256]; unsigned images = 0;
    unsigned pages = paginate(output, sizeof(output), NULL, &images);
    assert(pages >= 2 && images == 1 && !strcmp(output, "prefaceChapter"));
    assert(view.toc_count == 1 && document_view_toc_seek(0));
    document_progress(NULL, &view.progress);
    for (unsigned i = 0; view.link_target[0] && i < 20; ++i) link_process();
    assert(!view.link_target[0] && view.restoring && view.anchor.record == target);
}
static void test_thin_image_scaling(void)
{
    setup();
    view.view.margin = 44;
    emit_image(620, 1);
    stream_complete = true;
    view_page_t *page = calloc(1, sizeof(*page));
    assert(page);
    assert(test_step(page, false) == 1);
    assert(page->count == 1 && page->runs[0].width == 596 && page->runs[0].height == 1);
    free(page);
}


static void test_legacy_equivalence(void)
{
    setup();
    const char chapter[] = "chapter\0Title\0";
    emit(DOCUMENT_ANCHOR, 3, chapter, sizeof(chapter));
    emit(DOCUMENT_TEXT, 0, "prefix inter", 12);
    emit(DOCUMENT_TEXT, 0, "nationalization\t\n", 17);
    emit(DOCUMENT_PARAGRAPH, 0, NULL, 0);
    emit(DOCUMENT_PARAGRAPH, 0, NULL, 0);
    static const char sample[] = "ab entire-word 中文\tend\r\nlonger English phrase ";
    for (unsigned i = 0; i < 160; ++i)
    {
        unsigned styles[] = {0, BOOK_STYLE_BOLD, BOOK_STYLE_ITALIC, BOOK_STYLE_CENTER,
            BOOK_STYLE_RIGHT, BOOK_STYLE_PREFORMAT, BOOK_STYLE_HEADING};
        char text[DOCUMENT_TEXT_LIMIT]; unsigned bytes = 0;
        for (unsigned n = 0; n < 13; ++n)
        { memcpy(text + bytes, sample, sizeof(sample) - 1); bytes += sizeof(sample) - 1; }
        uint32_t offset = emit(DOCUMENT_TEXT, styles[i % 7], text, bytes);
        document_record_t record;
        memcpy(&record, spool + offset, sizeof(record)); record.link = i % 4 ? 0 : 27;
        memcpy(spool + offset, &record, sizeof(record));
        if (i % 8 == 0) emit(DOCUMENT_PARAGRAPH, 0, NULL, 0);
        if (i % 13 == 0)
            emit_image(620, i % 26 ? 1040 : 1);
    }
    stream_complete = true;
    view_page_t *old = calloc(1, sizeof(*old)), *page = calloc(1, sizeof(*page)), *boundary = calloc(1, sizeof(*boundary));
    document_cursor_t start = {0, 0};
    unsigned old_queries = 0, new_queries = 0, pages = 0;
    do
    {
        page_reset(old, start); page_reset(page, start); page_reset(boundary, start); boundary->collect = false;
        unsigned before = glyph_queries;
        while (!old->done) assert(legacy_page_step(old, false) == 1);
        old_queries += glyph_queries - before; before = glyph_queries;
        document_progress_t progress; document_progress(NULL, &progress);
        while (!page->done) assert(page_step(page, false, NULL, &progress) == 1);
        new_queries += glyph_queries - before;
        while (!boundary->done) assert(page_step(boundary, false, NULL, &progress) == 1);
        assert(cursor_equal(old->cursor, page->cursor) && cursor_equal(page->cursor, boundary->cursor));
        assert(old->count == page->count && old->bytes == page->bytes && old->eof == page->eof);
        assert(!memcmp(old->runs, page->runs, page->count * sizeof(*page->runs)));
        assert(!memcmp(old->text, page->text, page->bytes));
        assert(boundary->count == page->count && boundary->bytes == page->bytes);
        assert(!memcmp(boundary->runs, page->runs, page->count * sizeof(*page->runs)));
        start = old->cursor; ++pages;
    } while (!old->eof);
    assert(new_queries < old_queries);
    printf("Legacy comparison: %u pages, glyph queries %u -> %u\n", pages, old_queries, new_queries);
    free(old); free(page); free(boundary);
}
static void test_snapshot_boundary(void)
{
    setup();
    emit(DOCUMENT_TEXT, 0, "first", 5);
    document_progress_t snapshot; document_progress(NULL, &snapshot);
    emit(DOCUMENT_TEXT, 0, "next", 4);
    view_page_t *page = calloc(1, sizeof(*page)); page_reset(page, (document_cursor_t){0, 0});
    while (page->cursor.record < snapshot.end) assert(page_step(page, false, NULL, &snapshot) == 1);
    assert(page_step(page, false, NULL, &snapshot) == 0 && !page->done);
    stream_complete = true; document_progress(NULL, &snapshot);
    while (!page->done) assert(page_step(page, false, NULL, &snapshot) == 1);
    assert(!strcmp(page->text, "firstnext"));
    free(page);
}
static void test_image_info_validation(void)
{
    view_page_t *page = calloc(1, sizeof(*page)); assert(page);
    for (unsigned scenario = 0; scenario < INVALID_IMAGE_CASES; ++scenario)
    {
        setup(); emit_invalid_image(scenario); stream_complete = true;
        page_reset(page, (document_cursor_t){0, 0});
        assert(test_step(page, true) == -1);
        assert(image_info_calls == 1 && image_info_invalidations == 1 && !page->count);
        assert(!page->cursor.record && !page->cursor.byte);
    }
    setup(); emit_image(3, 3); stream_complete = true;
    page_reset(page, (document_cursor_t){0, 0});
    assert(test_step(page, true) == 1 && page->count == 1);
    assert(image_info_calls == 1 && !image_info_invalidations);
    assert(page->runs[0].width == 3 && page->runs[0].height == 3);
    free(page);
}

int main(void)
{
    test_streaming_and_styles(); test_pages_and_reflow(); test_toc_definition_and_image();
    test_thin_image_scaling(); test_legacy_equivalence(); test_snapshot_boundary(); test_image_info_validation();
    free(view.toc);
    puts("PASS: streaming, UTF-8, styles, pagination, reflow anchors, deferred TOC, images, legacy page/run/text equivalence, frozen publication boundary, image-info validation/invalidation");
    return 0;
}
