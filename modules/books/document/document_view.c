/* SPDX-License-Identifier: Apache-2.0 */
#include "document_view.h"
#include "document_font.h"
#include "../ui_bookshelf_data.h"
#include "ui_settings.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <ctype.h>

#define VIEW_TEXT 16384u
#define VIEW_RUNS 768u
#define VIEW_HEIGHT (UI_READER_BOTTOM - UI_READER_TOP)

typedef struct {
    uint16_t x, y, width, height, text, bytes;
    uint32_t style, link, image;
} view_run_t;
typedef struct {
    document_cursor_t start, cursor;
    unsigned x, y, line_height, line_start, line_style;
    unsigned count, bytes;
    view_run_t runs[VIEW_RUNS];
    char text[VIEW_TEXT];
    uint32_t loaded;
    document_record_t record;
    char data[DOCUMENT_TEXT_LIMIT + 1];
    bool has_record, done, eof;
} view_page_t;
typedef struct { uint32_t record; unsigned level; } toc_entry_t;
typedef struct {
    view_page_t *page;
    unsigned number, warm_run, warm_byte;
    uint32_t layout;
    bool valid;
} neighbour_t;
typedef struct view_image {
    struct view_image *next;
    document_bitmap_t bitmap;
} view_image_t;
static struct {
    document_t *document;
    ui_reader_view_t view;
    char title[BOOKSHELF_NAME_MAX];
    unsigned book, wanted, count, capacity, toc_count, toc_capacity;
    uint32_t toc_scanned, layout, display_layout;
    bool complete, restoring, display_pending;
    rt_tick_t opened;
    document_cursor_t anchor;
    document_cursor_t *pages;
    toc_entry_t *toc;
    view_page_t *index, *building, *display;
    neighbour_t neighbours[2];
    lv_font_t *fonts[3], *fallbacks[3], *retired[6];
    lv_font_t *oblique[3], *retired_oblique[3];
    unsigned settings[UI_SETTING_COUNT - UI_SETTING_FONT];
    lv_obj_t *root;
    view_image_t *images;
    char link_target[DOCUMENT_TEXT_LIMIT + 1];
    uint32_t link_scan;
} view;

static void fail(const char *message)
{
    if (!strcmp(view.view.error, message)) return;
    snprintf(view.view.error, sizeof(view.view.error), "%s", message);
    view.view.indexing = false;
    ++view.view.revision;
}
static bool cursor_before(document_cursor_t a, document_cursor_t b)
{ return a.record < b.record || (a.record == b.record && a.byte < b.byte); }
static bool cursor_equal(document_cursor_t a, document_cursor_t b)
{ return a.record == b.record && a.byte == b.byte; }
static unsigned font_slot(unsigned style)
{ return style & BOOK_STYLE_HEADING ? 2 : style & BOOK_STYLE_BOLD ? 1 : 0; }
static const lv_font_t *font_for(unsigned style)
{
    unsigned slot = font_slot(style);
    return (style & BOOK_STYLE_ITALIC) && view.oblique[slot] ? view.oblique[slot] : view.fonts[slot];
}
static unsigned width_available(void) { return 684 - view.view.margin * 2; }
static void page_reset(view_page_t *p, document_cursor_t start)
{
    memset(p, 0, sizeof(*p));
    p->start = p->cursor = start;
}
static bool reserve_pages(unsigned n)
{
    if (n > UI_READER_PAGE_MAX + 1 || n > SIZE_MAX / sizeof(*view.pages)) return false;
    if (n <= view.capacity) return true;
    unsigned capacity = view.capacity ? view.capacity * 2 : 256;
    if (capacity < n) capacity = n;
    if (capacity > SIZE_MAX / sizeof(*view.pages)) capacity = n;
    void *data = epd_app_realloc(view.pages, capacity * sizeof(*view.pages), EPD_APP_PSRAM);
    if (!data) return false;
    view.pages = data; view.capacity = capacity;
    return true;
}
static bool add_toc(uint32_t record, unsigned level)
{
    if (view.toc_count == view.toc_capacity)
    {
        if (view.toc_count >= SIZE_MAX / sizeof(*view.toc))
        { fail("目录索引超出寻址范围"); return false; }
        unsigned capacity = view.toc_capacity ? view.toc_capacity * 2 : 32;
        if (capacity > SIZE_MAX / sizeof(*view.toc)) capacity = view.toc_count + 1;
        void *p = epd_app_realloc(view.toc, capacity * sizeof(*view.toc), EPD_APP_PSRAM);
        if (!p) { fail("目录内存不足"); return false; }
        view.toc = p; view.toc_capacity = capacity;
    }
    view.toc[view.toc_count++] = (toc_entry_t){record, level};
    return true;
}
static void line_finish(view_page_t *p)
{
    unsigned delta = p->line_style & BOOK_STYLE_CENTER ? (width_available() - p->x) / 2 :
                     p->line_style & BOOK_STYLE_RIGHT ? width_available() - p->x : 0;
    for (unsigned i = p->line_start; i < p->count; ++i) p->runs[i].x += delta;
    p->y += (p->line_height ? p->line_height : view.fonts[0]->line_height) + view.view.line_space;
    p->x = 0; p->line_height = 0; p->line_start = p->count; p->line_style = 0;
}
static unsigned utf8_letter(const char *p, unsigned n, uint32_t *letter)
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
static bool append_letter(view_page_t *p, const char *text, unsigned bytes, uint32_t letter)
{
    unsigned style = p->record.style;
    const lv_font_t *font = font_for(style);
    lv_font_glyph_dsc_t glyph;
    memset(&glyph, 0, sizeof(glyph));
    lv_font_get_glyph_dsc(font, &glyph, letter == '\t' ? ' ' : letter, 0);
    unsigned width = glyph.adv_w * (letter == '\t' ? 4 : 1);
    if (width > width_available()) width = width_available();
    if (p->x && p->x + width > width_available()) line_finish(p);
    if (p->y + font->line_height > VIEW_HEIGHT) { p->done = true; return false; }
    if (p->bytes + (letter == '\t' ? 4 : bytes) + 2 >= VIEW_TEXT || p->count >= VIEW_RUNS - 1)
    { if (p->x) line_finish(p); p->done = true; return false; }
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
static int page_step(view_page_t *p, bool indexing)
{
    document_status_t status;
    document_status(view.document, &status);
    if (p->done) return 1;
    if (!p->has_record || p->loaded != p->cursor.record)
    {
        if (p->cursor.record == status.end)
        {
            if (!status.complete) return 0;
            if (p->x) line_finish(p);
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
            unsigned n = utf8_letter(p->data + p->cursor.byte, p->record.length - p->cursor.byte, &letter);
            if (letter == '\r') { p->cursor.byte += n; return 1; }
            if (letter == '\n') { line_finish(p); p->cursor.byte += n; return 1; }
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
                if (word_width <= width_available() && p->x + word_width > width_available()) line_finish(p);
            }
            if (!append_letter(p, p->data + p->cursor.byte, n, letter)) return 1;
            p->cursor.byte += n;
            return 1;
        }
        break;
    case DOCUMENT_PARAGRAPH:
        if (p->x) line_finish(p);
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
        if (p->x) line_finish(p);
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

static void save_position(void)
{
    if (!view.view.ready) return;
    const ui_bookshelf_book_t *book = ui_bookshelf_book(view.book);
    if (!book) return;
    ui_reading_position_t p = {0};
    p.current_page = view.view.page; p.total_pages = view.view.pages;
    p.progress = view.view.percent; p.layout = view.layout;
    p.document_version = DOCUMENT_CONVERSION_VERSION; p.document_kind = book->file.format;
    if (view.display)
    { p.offset = view.display->start.record; p.document_byte = view.display->start.byte; }
    ui_reading_position_t old = book->position;
    old.last_read = p.last_read = 0;
    if (view.view.saved && !memcmp(&old, &p, sizeof(p))) return;
    time_t now = time(NULL);
    if (now >= 1704067200) p.last_read = (uint32_t)now;
    view.view.saved = ui_bookshelf_save(view.book, &p);
    epd_app_set_recent_reading(view.title, p.progress);
}

static bool prepare_fonts(void)
{
    static const unsigned sizes[] = {16, 18, 20, 22, 24, 28, 32, 36};
    static const unsigned margins[] = {4, 8, 12, 16, 20};
    lv_font_t *fonts[3] = {0}, *fallbacks[3] = {0};
    lv_font_t *oblique[3] = {0};
    uint32_t identity = 0;
    unsigned size = sizes[ui_settings_index(UI_SETTING_FONT_SIZE)];
    for (unsigned i = 0; i < 3; ++i)
    {
        fonts[i] = epd_app_font_create(ui_settings_index(UI_SETTING_FONT), size + (i == 2 ? 6 : 0),
                     i ? 1 : ui_settings_index(UI_SETTING_FONT_WEIGHT), &identity, &fallbacks[i]);
        if (fonts[i]) oblique[i] = document_font_oblique(fonts[i]);
        if (!fonts[i] || !oblique[i])
        {
            for (unsigned j = 0; j <= i; ++j)
            { document_font_free(oblique[j]); epd_app_font_destroy(fonts[j]); epd_app_font_destroy(fallbacks[j]); }
            return false;
        }
        fonts[i]->kerning = LV_FONT_KERNING_NONE;
        if (fallbacks[i]) fallbacks[i]->kerning = LV_FONT_KERNING_NONE;
    }
    for (unsigned i = 0; i < 3; ++i)
    {
        if (view.retired[i])
        { document_font_free(view.oblique[i]); epd_app_font_destroy(view.fonts[i]); epd_app_font_destroy(view.fallbacks[i]); }
        else { view.retired[i] = view.fonts[i]; view.retired[i + 3] = view.fallbacks[i]; view.retired_oblique[i] = view.oblique[i]; }
        view.fonts[i] = fonts[i]; view.fallbacks[i] = fallbacks[i];
        view.oblique[i] = oblique[i];
    }
    view.view.font = view.fonts[0];
    view.view.margin = 24 + margins[ui_settings_index(UI_SETTING_MARGIN)];
    view.view.line_space = view.fonts[0]->line_height * ui_settings_index(UI_SETTING_LINE_SPACING) / 5;
    uint32_t layout = identity ^ 0x42564401u;
    for (unsigned i = UI_SETTING_FONT; i < UI_SETTING_COUNT; ++i)
    {
        view.settings[i - UI_SETTING_FONT] = ui_settings_index(i);
        layout = (layout ^ ui_settings_index(i)) * 16777619u;
    }
    view.layout = layout;
    return true;
}

bool document_view_open(unsigned book_index)
{
    document_view_close();
    const ui_bookshelf_book_t *book = ui_bookshelf_book(book_index);
    if (!book) return false;
    view.book = book_index;
    view.opened = rt_tick_get();
    snprintf(view.title, sizeof(view.title), "%s", book->file.name);
    view.view.title = view.title; view.view.text = "";
    view.view.margin = 32; view.view.font = epd_app_font_role(EPD_FONT_BODY);
    view.document = document_open(&book->file, view.view.error, sizeof(view.view.error));
    if (!view.document) return false;
    view.index = epd_app_alloc(sizeof(*view.index), EPD_APP_PSRAM);
    view.building = epd_app_alloc(sizeof(*view.building), EPD_APP_PSRAM);
    view.display = epd_app_alloc(sizeof(*view.display), EPD_APP_PSRAM);
    if (!view.index || !view.building || !view.display || !reserve_pages(2) || !prepare_fonts())
    { document_view_close(); snprintf(view.view.error, sizeof(view.view.error), "字体或排版工作内存不足"); return false; }
    view.pages[0] = (document_cursor_t){0, 0};
    page_reset(view.index, view.pages[0]); page_reset(view.building, view.pages[0]); page_reset(view.display, view.pages[0]);
    if (book->position.document_version == DOCUMENT_CONVERSION_VERSION && book->position.document_kind == book->file.format)
        view.anchor = (document_cursor_t){book->position.offset, book->position.document_byte};
    view.restoring = true; view.view.indexing = true;
    ++view.view.revision;
    return true;
}

static void images_free(view_image_t *images)
{
    while (images)
    {
        view_image_t *next = images->next;
        epd_app_free(images->bitmap.memory); epd_app_free(images); images = next;
    }
}
static void root_deleted(lv_event_t *event)
{
    if (view.root == lv_event_get_target_obj(event)) view.root = NULL;
}
void document_view_detach(void)
{
    lv_draw_wait_for_finish();
    if (view.root) { lv_obj_delete(view.root); view.root = NULL; }
    images_free(view.images); view.images = NULL;
}
void document_view_close(void)
{
    if (view.document) save_position();
    document_view_detach();
    document_close(view.document);
    for (unsigned i = 0; i < 3; ++i)
    { document_font_free(view.oblique[i]); document_font_free(view.retired_oblique[i]);
      epd_app_font_destroy(view.fonts[i]); epd_app_font_destroy(view.fallbacks[i]); }
    for (unsigned i = 0; i < 6; ++i) epd_app_font_destroy(view.retired[i]);
    epd_app_free(view.pages); epd_app_free(view.toc);
    epd_app_free(view.index); epd_app_free(view.building); epd_app_free(view.display);
    for (unsigned i = 0; i < 2; ++i) epd_app_free(view.neighbours[i].page);
    memset(&view, 0, sizeof(view));
}
bool document_view_active(void) { return view.document != NULL; }
const ui_reader_view_t *document_view_get(void) { return &view.view; }

static void link_process(void)
{
    if (!view.link_target[0]) return;
    document_status_t status;
    document_status(view.document, &status);
    rt_tick_t start = rt_tick_get();
    while (view.link_scan < status.end)
    {
        document_record_t r;
        if (!document_record(view.document, view.link_scan, &r)) { fail("链接索引读取失败"); return; }
        if (r.kind == DOCUMENT_ANCHOR && r.length <= DOCUMENT_TEXT_LIMIT)
        {
            char text[DOCUMENT_TEXT_LIMIT + 1];
            if (!document_read(view.document, view.link_scan + sizeof(r), text, r.length)) return;
            text[r.length] = 0;
            size_t id_length = strlen(text);
            bool definition = id_length + 1 < r.length && !text[id_length + 1];
            if (definition && (!strcmp(text, view.link_target) ||
                (view.link_target[0] == '#' && !strcmp(text, view.link_target + 1))))
            {
                view.anchor = (document_cursor_t){view.link_scan, 0};
                view.restoring = true; view.link_target[0] = 0; return;
            }
        }
        view.link_scan += sizeof(r) + r.length;
        if (rt_tick_get() - start >= rt_tick_from_millisecond(2)) return;
    }
    if (status.complete) { view.link_target[0] = 0; fail("未找到文档内部链接目标"); }
}

void document_view_process(bool foreground)
{
    if (!view.document) return;
    document_status_t status;
    document_status(view.document, &status);
    if (status.title[0]) snprintf(view.title, sizeof(view.title), "%s", status.title);
    if (status.error[0]) { fail(status.error); return; }
    if (view.view.error[0]) return;
    link_process();
    rt_tick_t start = rt_tick_get(), budget = rt_tick_from_millisecond(8);
    if (!budget) budget = 1;
    do
    {
        if (view.restoring && view.count && (view.complete || cursor_before(view.anchor, view.pages[view.count])))
        {
            unsigned lo = 0, hi = view.count;
            while (lo + 1 < hi)
            {
                unsigned mid = lo + (hi - lo) / 2;
                if (!cursor_before(view.anchor, view.pages[mid])) lo = mid; else hi = mid;
            }
            view.wanted = lo; view.restoring = false;
        }
        if (view.complete && view.wanted >= view.count) view.wanted = view.count ? view.count - 1 : 0;
        if (foreground && !view.restoring && view.wanted < view.count && !view.display_pending &&
            (!view.view.ready || view.display_layout != view.layout || view.view.page != view.wanted + 1 ||
             !cursor_equal(view.display->start, view.pages[view.wanted])))
        {
            for (unsigned i = 0; i < 2; ++i)
            {
                neighbour_t *cached = &view.neighbours[i];
                if (cached->valid && cached->number == view.wanted && cached->layout == view.layout &&
                    cursor_equal(cached->page->start, view.pages[view.wanted]))
                {
                    view_page_t *old = view.building;
                    view.building = cached->page; cached->page = old; cached->valid = false; cached->layout = 0;
                    view.display_pending = true; ++view.view.revision;
                    break;
                }
            }
            if (view.display_pending) break;
            if (!cursor_equal(view.building->start, view.pages[view.wanted]) || view.building->done)
                page_reset(view.building, view.pages[view.wanted]);
            while (!view.building->done)
            {
                int result = page_step(view.building, false);
                if (result < 0) { fail("文档页面读取失败"); return; }
                if (!result) return;
                /* Publish a finished page even when the last step used the
                 * remaining time slice; otherwise the next call restarts it. */
                if (!view.building->done && rt_tick_get() - start >= budget) return;
            }
            view.display_pending = true; ++view.view.revision;
            break;
        }
        if (view.complete || view.display_pending) break;
        int result = page_step(view.index, true);
        if (result < 0) { fail("文档排版失败"); return; }
        if (!result) break;
        if (view.index->done)
        {
            if (!view.index->count && view.index->eof && view.count)
            { view.complete = true; break; }
            if (!reserve_pages(view.count + 2)) { fail("分页索引内存不足"); return; }
            view.pages[++view.count] = view.index->cursor;
            view.complete = view.index->eof;
            if (!view.complete) page_reset(view.index, view.index->cursor);
        }
    } while (rt_tick_get() - start < budget);
    view.view.indexing = !view.complete;
    if (view.complete)
    {
        view.view.pages = view.count;
        /* The page revision stays unchanged: the caller updates only its footer. */
        if (view.view.ready) save_position();
    }
}

void document_view_prepare(void)
{
    if (!view.document || document_view_waiting() || view.view.error[0]) return;
    lv_draw_wait_for_finish();
    rt_tick_t started = rt_tick_get(), budget = rt_tick_from_millisecond(8);
    if (!budget) budget = 1;
    unsigned desired[] = {view.view.page, view.view.page > 1 ? view.view.page - 2 : UINT_MAX};
    for (unsigned slot = 0; slot < 2; ++slot)
    {
        neighbour_t *n = &view.neighbours[slot];
        if (desired[slot] >= view.count) continue;
        if (!n->page)
        {
            n->page = epd_app_alloc(sizeof(*n->page), EPD_APP_PSRAM);
            if (!n->page) return;
            n->layout = 0;
        }
        if (n->layout != view.layout || n->number != desired[slot])
        {
            n->layout = view.layout; n->number = desired[slot]; n->valid = false;
            n->warm_run = n->warm_byte = 0;
            page_reset(n->page, view.pages[n->number]);
        }
        while (!n->page->done)
        {
            int result = page_step(n->page, false);
            if (result <= 0) return;
            if (rt_tick_get() - started >= budget) return;
        }
        n->valid = cursor_equal(n->page->cursor, view.pages[n->number + 1]);
        if (!n->valid) return;
        while (n->warm_run < n->page->count)
        {
            view_run_t *run = &n->page->runs[n->warm_run];
            if (run->image || n->warm_byte >= run->bytes)
            { ++n->warm_run; n->warm_byte = 0; continue; }
            uint32_t letter;
            n->warm_byte += utf8_letter(n->page->text + run->text + n->warm_byte, run->bytes - n->warm_byte, &letter);
            /* The oblique wrapper borrows these same source glyph bitmaps. */
            epd_app_font_prepare(view.fonts[font_slot(run->style)], letter, true);
            if (rt_tick_get() - started >= budget) return;
        }
    }
}

static void link_clicked(lv_event_t *event)
{
    if (!epd_app_refresh_done() || document_view_waiting()) return;
    document_view_follow_link((uint32_t)(uintptr_t)lv_event_get_user_data(event));
}
bool document_view_bind(lv_obj_t *body)
{
    if (!view.document) return false;
    if (!view.display_pending && view.root) return true;
    if (!view.display_pending && !view.view.ready) return false;
    lv_obj_t *root = lv_obj_create(body);
    epd_app_object_init(root);
    lv_obj_set_size(root, width_available(), VIEW_HEIGHT);
    lv_obj_set_style_pad_all(root, 0, 0); lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0); lv_obj_set_style_bg_color(root, lv_color_white(), 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    view_image_t *images = NULL;
    view_page_t *page = view.display_pending ? view.building : view.display;
    for (unsigned i = 0; i < page->count; ++i)
    {
        view_run_t *run = &page->runs[i];
        lv_obj_t *object;
        if (run->image)
        {
            view_image_t *image = epd_app_alloc(sizeof(*image), EPD_APP_PSRAM);
            if (!image) goto failed;
            memset(image, 0, sizeof(*image)); image->next = images; images = image;
            if (!document_image_load(view.document, run->image - 1, run->width, run->height, &image->bitmap)) goto failed;
            object = lv_image_create(root);
            lv_image_set_src(object, &image->bitmap.image);
        }
        else
        {
            object = lv_label_create(root);
            lv_obj_set_style_text_font(object, font_for(run->style), 0);
            lv_obj_set_style_text_color(object, lv_color_black(), 0);
            lv_obj_set_style_text_line_space(object, 0, 0);
            lv_obj_set_style_text_letter_space(object, 0, 0);
            lv_label_set_long_mode(object, LV_LABEL_LONG_CLIP);
            lv_obj_set_size(object, run->width + 2, run->height);
            lv_label_set_text(object, page->text + run->text);
            if (run->link) lv_obj_set_style_text_decor(object, LV_TEXT_DECOR_UNDERLINE, 0);
        }
        lv_obj_set_pos(object, run->x, run->y);
        if (run->link)
        {
            lv_obj_add_flag(object, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(object, link_clicked, LV_EVENT_CLICKED, (void *)(uintptr_t)run->link);
        }
        else lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_draw_wait_for_finish();
    if (view.root) lv_obj_delete(view.root);
    images_free(view.images); view.images = images;
    view.root = root;
    lv_obj_set_pos(body, view.view.margin, UI_READER_TOP);
    lv_obj_set_size(body, width_available(), VIEW_HEIGHT);
    lv_obj_add_event_cb(root, root_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_HIDDEN);
    if (view.display_pending)
    {
        view_page_t *old = view.display; view.display = view.building; view.building = old;
        view.display_layout = view.layout;
        view.view.page = view.wanted + 1;
        document_status_t status;
        document_status(view.document, &status);
        view.view.percent = status.complete && status.end ?
            (unsigned)((uint64_t)view.display->cursor.record * 100 / status.end) : 0;
        if (!view.view.ready)
            rt_kprintf("[document] first-page-ready: page=%u, %lu ms\n", view.view.page,
                (unsigned long)((uint64_t)(rt_tick_get() - view.opened) * 1000 / RT_TICK_PER_SECOND));
        view.view.ready = true; view.display_pending = false;
        save_position();
    }
    for (unsigned i = 0; i < 6; ++i) { epd_app_font_destroy(view.retired[i]); view.retired[i] = NULL; }
    for (unsigned i = 0; i < 3; ++i) { document_font_free(view.retired_oblique[i]); view.retired_oblique[i] = NULL; }
    return true;
failed:
    lv_obj_delete(root); images_free(images);
    fail("当前页面图片所需内存不足或缓存读取失败");
    return false;
}

bool document_view_save(void) { save_position(); return ui_bookshelf_flush(); }
bool document_view_reflow(void)
{
    if (!view.document) return false;
    save_position();
    view.anchor = view.view.ready ? view.display->start : view.anchor;
    document_status_t status;
    document_status(view.document, &status);
    if (status.error[0])
    {
        const ui_bookshelf_book_t *book = ui_bookshelf_book(view.book);
        char error[96] = "书籍不可用，请返回书架";
        document_t *replacement = book ? document_open(&book->file, error, sizeof(error)) : NULL;
        if (!replacement) { fail(error); return false; }
        document_close(view.document);
        view.document = replacement;
        document_service_process();
        view.toc_count = view.toc_scanned = 0;
        view.opened = rt_tick_get();
    }
    if (!prepare_fonts()) { fail("字体内存不足"); return false; }
    view.view.error[0] = 0;
    view.link_target[0] = 0;
    view.link_scan = 0;
    view.count = 0; view.complete = false; view.restoring = true; view.display_pending = false;
    for (unsigned i = 0; i < 2; ++i) { view.neighbours[i].valid = false; view.neighbours[i].layout = 0; }
    page_reset(view.index, (document_cursor_t){0, 0});
    page_reset(view.building, (document_cursor_t){0, 0});
    view.view.pages = 0; view.view.indexing = true;
    ++view.view.revision;
    return true;
}
bool document_view_settings_changed(void)
{
    for (unsigned i = UI_SETTING_FONT; i < UI_SETTING_COUNT; ++i)
        if (view.settings[i - UI_SETTING_FONT] != ui_settings_index(i)) return true;
    return false;
}
void document_view_seek(unsigned page)
{
    if (!page || !view.document || view.view.error[0]) return;
    if (view.view.pages && page > view.view.pages) page = view.view.pages;
    if (page > UI_READER_PAGE_MAX) page = UI_READER_PAGE_MAX;
    view.restoring = false; view.wanted = page - 1;
    view.display_pending = false;
    page_reset(view.building, view.wanted < view.count ? view.pages[view.wanted] : (document_cursor_t){0, 0});
}
void document_view_cancel_pending(void)
{
    if (view.view.ready) document_view_seek(view.view.page);
}
bool document_view_waiting(void)
{
    return !view.view.error[0] && (!view.view.ready || view.restoring || view.link_target[0] ||
        view.wanted + 1 != view.view.page || view.display_pending ||
        view.display_layout != view.layout);
}
unsigned document_view_toc_count(void) { return view.toc_count; }
bool document_view_toc_title(unsigned index, char *title, size_t size, unsigned *level)
{
    if (index >= view.toc_count) return false;
    document_record_t r;
    char data[DOCUMENT_TEXT_LIMIT + 1];
    if (!document_record(view.document, view.toc[index].record, &r) || r.length > DOCUMENT_TEXT_LIMIT ||
        !document_read(view.document, view.toc[index].record + sizeof(r), data, r.length)) return false;
    data[r.length] = 0;
    size_t start = strlen(data) + 1;
    if (start >= r.length) return false;
    snprintf(title, size, "%s", data + start);
    if (level) *level = view.toc[index].level;
    return true;
}
bool document_view_toc_seek(unsigned index)
{
    if (index >= view.toc_count) return false;
    document_record_t r;
    if (!document_record(view.document, view.toc[index].record, &r) || r.length > DOCUMENT_TEXT_LIMIT ||
        !document_read(view.document, view.toc[index].record + sizeof(r), view.link_target, r.length)) return false;
    view.link_target[r.length] = 0;
    view.link_scan = 0;
    view.display_pending = false;
    return true;
}
bool document_view_follow_link(uint32_t record)
{
    document_record_t r;
    if (!record || !document_record(view.document, record - 1, &r) || r.kind != DOCUMENT_LINK || r.length > DOCUMENT_TEXT_LIMIT ||
        !document_read(view.document, record - 1 + sizeof(r), view.link_target, r.length)) return false;
    view.link_target[r.length] = 0;
    if (strstr(view.link_target, "://") || !strncmp(view.link_target, "mailto:", 7))
    { view.link_target[0] = 0; return false; }
    view.link_scan = 0;
    return true;
}
