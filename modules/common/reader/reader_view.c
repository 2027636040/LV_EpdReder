/* SPDX-License-Identifier: Apache-2.0 */
#include "reader_internal.h"
#include "reader_font.h"
#include "ui_settings.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#define VIEW_TEXT 16384u
#define VIEW_RUNS 768u
#define VIEW_HEIGHT (READER_BOTTOM - READER_TOP)
#define VIEW_BLOCK 64u
#define VIEW_INDEX_MAGIC 0x58445642u
#define VIEW_INDEX_VERSION 1u
#define VIEW_LAYOUT_VERSION 2u
#ifndef DOCUMENT_VIEW_TRACE
#define DOCUMENT_VIEW_TRACE 1
#endif

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
    struct { uint32_t letter, width; unsigned bytes; } decoded[DOCUMENT_TEXT_LIMIT];
    bool has_record, done, eof, collect;
} view_page_t;
typedef struct { uint32_t record; unsigned level; } toc_entry_t;
typedef struct {
    view_page_t *page;
    unsigned number, warm_run, warm_byte;
    uint32_t layout;
    bool valid, unavailable;
} neighbour_t;
typedef struct view_image {
    struct view_image *next;
    document_t *document;
    document_bitmap_t bitmap;
    document_image_request_t *request;
    uint32_t layout, record;
    unsigned width, height;
    int state;
    bool bound, retried;
} view_image_t;
typedef struct view_widget {
    struct view_widget *next;
    lv_obj_t *object;
    uint32_t link;
    bool image, used;
} view_widget_t;
typedef struct {
    uint32_t magic, version, layout, end, count, toc_count, checksum;
} view_index_header_t;
static struct {
    document_t *document;
    document_reader_t *index_reader, *page_reader, *aux_reader;
    document_progress_t progress;
    reader_view_t view;
    char title[DOCUMENT_TITLE_MAX];
    unsigned wanted, count, capacity, toc_count, toc_capacity;
    reader_save_fn save;
    document_t *(*reopen)(void *user, char *error, size_t size);
    void *user;
    reader_position_t saved_position;
    uint32_t toc_scanned, layout, display_layout;
    bool complete, restoring, display_pending, notified, index_loaded, index_saved, from_cache;
    int direction;
    char failure[96];
    rt_tick_t opened;
    rt_tick_t requested;
    uint32_t blocks, characters, measurements, layouts, reused, index_hits;
    bool prepared_logged;
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
    view_widget_t *widgets;
    char link_target[DOCUMENT_TEXT_LIMIT + 1];
    uint32_t link_scan;
} view;

static void fail(const char *message)
{
    snprintf(view.failure, sizeof(view.failure), "%s", message);
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
    p->collect = true;
}
static bool reserve_pages(unsigned n)
{
    if (n > READER_PAGE_MAX + 1 || n > SIZE_MAX / sizeof(*view.pages)) return false;
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
static void decode_at(view_page_t *p, unsigned at)
{
    if (p->decoded[at].bytes) return;
    uint32_t letter;
    unsigned bytes = utf8_letter(p->data + at, p->record.length - at, &letter);
    ++view.characters;
    lv_font_glyph_dsc_t glyph;
    memset(&glyph, 0, sizeof(glyph));
    if (letter != '\r' && letter != '\n')
    {
        lv_font_get_glyph_dsc(font_for(p->record.style), &glyph, letter == '\t' ? ' ' : letter, 0);
        ++view.measurements;
    }
    p->decoded[at].letter = letter;
    p->decoded[at].bytes = bytes;
    p->decoded[at].width = glyph.adv_w;
}
static bool append_letter(view_page_t *p, const char *text, unsigned bytes, uint32_t letter, unsigned advance)
{
    unsigned style = p->record.style;
    const lv_font_t *font = font_for(style);
    unsigned width = advance * (letter == '\t' ? 4 : 1);
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
    else if (p->bytes) --p->bytes;
    if (letter == '\t')
    {
        if (p->collect) memcpy(p->text + p->bytes, "    ", 4);
        p->bytes += 4; run->bytes += 4;
    }
    else { if (p->collect) memcpy(p->text + p->bytes, text, bytes); p->bytes += bytes; run->bytes += bytes; }
    if (p->collect) p->text[p->bytes] = 0;
    ++p->bytes;
    run->width += width; run->height = font->line_height;
    p->x += width;
    if (font->line_height > p->line_height) p->line_height = font->line_height;
    p->line_style |= style;
    return true;
}

/* Record offsets and character wrapping are shared by collected and index-only pages. */
static int page_event(view_page_t *p, bool indexing, document_reader_t *reader,
                      const document_progress_t *status)
{
    if (p->done) return 1;
    if (!p->has_record || p->loaded != p->cursor.record)
    {
        if (p->cursor.record == status->end)
        {
            if (!status->complete) return 0;
            if (p->x) line_finish(p);
            p->done = p->eof = true;
            return 1;
        }
        if (p->cursor.record > status->end || status->end - p->cursor.record < sizeof(document_record_t) ||
            !document_reader_record(reader, p->cursor.record, &p->record) ||
            p->record.length > status->end - p->cursor.record - sizeof(document_record_t)) return -1;
        p->loaded = p->cursor.record; p->has_record = true;
        if (p->record.kind < DOCUMENT_TEXT || p->record.kind > DOCUMENT_IMAGE ||
            (p->cursor.byte && p->record.kind != DOCUMENT_TEXT) || p->cursor.byte > p->record.length) return -1;
        if (p->record.kind != DOCUMENT_IMAGE)
        {
            if (p->record.length > DOCUMENT_TEXT_LIMIT ||
                !document_reader_read(reader, p->cursor.record + sizeof(document_record_t), p->data, p->record.length)) return -1;
            p->data[p->record.length] = 0;
            if (p->cursor.byte < p->record.length && p->cursor.byte &&
                ((unsigned char)p->data[p->cursor.byte] & 0xc0) == 0x80) return -1;
            memset(p->decoded, 0, sizeof(p->decoded));
        }
    }
    switch (p->record.kind)
    {
    case DOCUMENT_TEXT:
        if (p->cursor.byte < p->record.length)
        {
            decode_at(p, p->cursor.byte);
            uint32_t letter = p->decoded[p->cursor.byte].letter;
            unsigned n = p->decoded[p->cursor.byte].bytes;
            if (letter == '\r') { p->cursor.byte += n; return 1; }
            if (letter == '\n') { line_finish(p); p->cursor.byte += n; return 1; }
            if (!(p->record.style & BOOK_STYLE_PREFORMAT) && p->x && letter < 128 &&
                isalnum((unsigned char)letter) && (!p->cursor.byte ||
                isspace((unsigned char)p->data[p->cursor.byte - 1])))
            {
                unsigned word_width = 0;
                for (unsigned at = p->cursor.byte; at < p->record.length; ++at)
                {
                    unsigned char c = p->data[at];
                    if (c >= 128 || isspace(c)) break;
                    decode_at(p, at);
                    word_width += p->decoded[at].width;
                    if (word_width > width_available()) break;
                }
                if (word_width <= width_available() && p->x + word_width > width_available()) line_finish(p);
            }
            if (!append_letter(p, p->data + p->cursor.byte, n, letter, p->decoded[p->cursor.byte].width)) return 1;
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
        if (!document_reader_image_info(reader, p->cursor.record, &image)) return -1;
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

static int page_step(view_page_t *p, bool indexing, document_reader_t *reader,
                     const document_progress_t *status)
{
    ++view.blocks;
    for (unsigned i = 0; i < VIEW_BLOCK; ++i)
    {
        uint32_t record = p->cursor.record;
        unsigned y = p->y;
        int result = page_event(p, indexing, reader, status);
        if (result <= 0 || p->done) { if (p->done) ++view.layouts; return result; }
        if (p->cursor.record != record || p->y != y) break;
    }
    return 1;
}

static void save_position(void)
{
    if (!view.view.ready) return;
    reader_position_t p = {0};
    p.page = view.view.page; p.pages = view.view.pages;
    p.percent = view.view.percent; p.layout = view.display_layout;
    p.conversion_version = DOCUMENT_CONVERSION_VERSION;
    if (view.display) p.cursor = view.display->start;
    if (view.view.saved && !memcmp(&view.saved_position, &p, sizeof(p))) return;
    view.view.saved = !view.save || view.save(view.user, &p);
    if (view.view.saved) view.saved_position = p;
}

static bool prepare_fonts(void)
{
    static const unsigned sizes[] = {16, 18, 20, 22, 24, 28, 32, 36};
    static const unsigned margins[] = {4, 8, 12, 16, 20};
    lv_font_t *fonts[3] = {0}, *fallbacks[3] = {0};
    lv_font_t *oblique[3] = {0};
    uint32_t identity = 0, font_identity = 2166136261u;
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
        font_identity = (font_identity ^ identity) * 16777619u;
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
    uint32_t layout = font_identity ^ VIEW_LAYOUT_VERSION;
    layout = (layout ^ width_available()) * 16777619u;
    layout = (layout ^ VIEW_HEIGHT) * 16777619u;
    layout = (layout ^ (unsigned)view.view.line_space) * 16777619u;
    for (unsigned i = UI_SETTING_FONT; i < UI_SETTING_COUNT; ++i)
    {
        view.settings[i - UI_SETTING_FONT] = ui_settings_index(i);
        layout = (layout ^ ui_settings_index(i)) * 16777619u;
    }
    view.layout = layout;
    return true;
}

static uint32_t index_checksum(const void *data, size_t size)
{
    const unsigned char *p = data;
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; ++i)
        if (i < offsetof(view_index_header_t, checksum) || i >= sizeof(view_index_header_t))
            hash = (hash ^ p[i]) * 16777619u;
    return hash;
}
static bool index_size(uint32_t count, uint32_t toc_count, size_t *size)
{
    if (!count || count > READER_PAGE_MAX ||
        (uint64_t)(count + 1u) * sizeof(document_cursor_t) +
        (uint64_t)toc_count * sizeof(toc_entry_t) > SIZE_MAX - sizeof(view_index_header_t)) return false;
    *size = sizeof(view_index_header_t) + (size_t)(count + 1u) * sizeof(document_cursor_t) +
            (size_t)toc_count * sizeof(toc_entry_t);
    return true;
}
static void index_load(void)
{
    if (view.index_loaded || !view.progress.complete) return;
    view.index_loaded = true;
    if (view.count || view.index->cursor.record || view.index->cursor.byte) return;
    void *blob = NULL;
    size_t size = 0, expected;
    if (!document_index_load(view.document, view.layout, &blob, &size)) return;
    if (!blob || size < sizeof(view_index_header_t)) goto done;
    view_index_header_t header;
    memcpy(&header, blob, sizeof(header));
    if (header.magic != VIEW_INDEX_MAGIC || header.version != VIEW_INDEX_VERSION || header.layout != view.layout ||
        header.end != view.progress.end || !index_size(header.count, header.toc_count, &expected) || expected != size ||
        header.checksum != index_checksum(blob, size)) goto done;
    document_cursor_t *pages = (document_cursor_t *)((char *)blob + sizeof(header));
    toc_entry_t *toc = (toc_entry_t *)(pages + header.count + 1u);
    if (pages[0].record || pages[0].byte || pages[header.count].record > header.end) goto done;
    for (unsigned i = 0; i <= header.count; ++i)
    {
        if (pages[i].record > header.end || pages[i].byte > DOCUMENT_TEXT_LIMIT ||
            (pages[i].record == header.end && pages[i].byte)) goto done;
        if (i && !cursor_before(pages[i - 1], pages[i]) &&
            !(header.count == 1 && !header.end && !pages[i - 1].record && !pages[i - 1].byte)) goto done;
    }
    for (unsigned i = 0; i < header.toc_count; ++i)
        if (toc[i].record >= header.end || (i && toc[i - 1].record >= toc[i].record)) goto done;
    toc_entry_t *copy = NULL;
    if (header.toc_count)
    {
        copy = epd_app_alloc((size_t)header.toc_count * sizeof(*copy), EPD_APP_PSRAM);
        if (!copy) goto done;
        memcpy(copy, toc, (size_t)header.toc_count * sizeof(*copy));
    }
    if (!reserve_pages(header.count + 1u)) { epd_app_free(copy); goto done; }
    memcpy(view.pages, pages, (size_t)(header.count + 1u) * sizeof(*pages));
    epd_app_free(view.toc); view.toc = copy;
    view.count = header.count; view.toc_count = view.toc_capacity = header.toc_count;
    view.toc_scanned = header.end; view.complete = view.index_saved = view.from_cache = true;
    ++view.index_hits;
done:
    epd_app_free(blob);
}
static void index_save(void)
{
    if (!view.complete || view.index_saved || !view.progress.complete || view.progress.result) return;
    view.index_saved = true;
    size_t size;
    if (!index_size(view.count, view.toc_count, &size)) return;
    view_index_header_t *blob = epd_app_alloc(size, EPD_APP_PSRAM);
    if (!blob) return;
    *blob = (view_index_header_t){VIEW_INDEX_MAGIC, VIEW_INDEX_VERSION, view.layout,
                                view.progress.end, view.count, view.toc_count, 0};
    document_cursor_t *pages = (document_cursor_t *)(blob + 1);
    memcpy(pages, view.pages, (size_t)(view.count + 1u) * sizeof(*pages));
    if (view.toc_count) memcpy(pages + view.count + 1u, view.toc, (size_t)view.toc_count * sizeof(*view.toc));
    blob->checksum = index_checksum(blob, size);
    document_index_save(view.document, view.layout, blob, size);
    epd_app_free(blob);
}
static bool readers_create(void)
{
    view.index_reader = document_reader_create(view.document);
    view.page_reader = document_reader_create(view.document);
    view.aux_reader = document_reader_create(view.document);
    return view.index_reader && view.page_reader && view.aux_reader;
}
static void readers_destroy(void)
{
    document_reader_destroy(view.index_reader); view.index_reader = NULL;
    document_reader_destroy(view.page_reader); view.page_reader = NULL;
    document_reader_destroy(view.aux_reader); view.aux_reader = NULL;
}

bool epd_reader_attach(document_t *document, const char *title,
                   const reader_position_t *restore, reader_save_fn save,
                   document_t *(*reopen)(void *user, char *error, size_t size), void *user)
{
    epd_reader_close();
    if (!document) return false;
    view.save = save; view.reopen = reopen; view.user = user;
    view.opened = view.requested = rt_tick_get();
    snprintf(view.title, sizeof(view.title), "%.*s", (int)sizeof(view.title) - 1, title ? title : "");
    view.view.title = view.title; view.view.text = "";
    view.view.margin = 32; view.view.font = epd_app_font_role(EPD_FONT_BODY);
    view.document = document;
    view.index = epd_app_alloc(sizeof(*view.index), EPD_APP_PSRAM);
    view.building = epd_app_alloc(sizeof(*view.building), EPD_APP_PSRAM);
    view.display = epd_app_alloc(sizeof(*view.display), EPD_APP_PSRAM);
    if (!view.index || !view.building || !view.display || !reserve_pages(2) || !prepare_fonts() || !readers_create())
    { epd_reader_close(); snprintf(view.view.error, sizeof(view.view.error), "字体或排版工作内存不足"); return false; }
    view.pages[0] = (document_cursor_t){0, 0};
    page_reset(view.index, view.pages[0]); page_reset(view.building, view.pages[0]); page_reset(view.display, view.pages[0]);
    if (restore && restore->conversion_version == DOCUMENT_CONVERSION_VERSION) view.anchor = restore->cursor;
    view.restoring = true; view.view.indexing = true; view.direction = 1;
    ++view.view.revision;
    return true;
}

static void images_free(view_image_t *images)
{
    while (images)
    {
        view_image_t *next = images->next;
        if (images->request) document_image_cancel(images->request);
        epd_app_free(images->bitmap.memory); epd_app_free(images); images = next;
    }
}
static void root_deleted(lv_event_t *event)
{
    if (view.root != lv_event_get_target_obj(event)) return;
    view.root = NULL;
    while (view.widgets)
    {
        view_widget_t *next = view.widgets->next;
        epd_app_free(view.widgets); view.widgets = next;
    }
    for (view_image_t *image = view.images; image; image = image->next) image->bound = false;
}
void epd_reader_detach(void)
{
    lv_draw_wait_for_finish();
    if (view.root) { lv_obj_delete(view.root); view.root = NULL; }
    images_free(view.images); view.images = NULL;
}
void epd_reader_close(void)
{
    if (view.document) save_position();
    epd_reader_detach();
    readers_destroy();
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
bool epd_reader_active(void) { return view.document != NULL; }
const reader_view_t *epd_reader_get(void) { return &view.view; }

static bool image_matches(const view_image_t *image, const view_run_t *run)
{
    return image->document == view.document && image->layout == view.layout && image->record == run->image - 1 &&
           image->width == run->width && image->height == run->height;
}
static bool page_uses_image(const view_page_t *page, const view_image_t *image)
{
    if (!page || !page->done || !page->collect) return false;
    for (unsigned i = 0; i < page->count; ++i)
        if (page->runs[i].image && image_matches(image, &page->runs[i])) return true;
    return false;
}
static bool target_needed(void)
{
    return !view.restoring && !view.link_target[0] && view.wanted < view.count &&
        (!view.view.ready || view.display_layout != view.layout || view.view.page != view.wanted + 1 ||
         !cursor_equal(view.display->start, view.pages[view.wanted]));
}
static void images_prune(bool optional)
{
    view_image_t **at = &view.images;
    while (*at)
    {
        view_image_t *image = *at;
        bool keep = image->bound || (view.display_pending && page_uses_image(view.building, image)) ||
                    (view.view.ready && view.display_layout == view.layout && page_uses_image(view.display, image));
        if (!optional)
        {
            if (view.view.ready && view.display_layout == view.layout && page_uses_image(view.display, image)) keep = true;
            for (unsigned i = 0; i < 2; ++i)
            {
                neighbour_t *n = &view.neighbours[i];
                if (n->valid && n->layout == view.layout &&
                    (n->number + 1 == view.wanted || n->number == view.wanted + 1) &&
                    page_uses_image(n->page, image)) keep = true;
            }
        }
        if (keep) at = &image->next;
        else
        {
            *at = image->next; image->next = NULL; images_free(image);
        }
    }
}
static view_image_t *image_find(const view_run_t *run)
{
    for (view_image_t *image = view.images; image; image = image->next)
        if (image_matches(image, run)) return image;
    return NULL;
}
static void images_cancel_pending(void)
{
    view_image_t **at = &view.images;
    while (*at)
    {
        view_image_t *image = *at;
        if (image->request && !image->bound)
        { *at = image->next; image->next = NULL; images_free(image); }
        else at = &image->next;
    }
}
/* Pending pixels are polled once per service call, never in the compute loop. */
static int images_prepare(view_page_t *page, bool required)
{
    int ready = 1;
    for (unsigned i = 0; i < page->count; ++i)
    {
        view_run_t *run = &page->runs[i];
        if (!run->image) continue;
        view_image_t *image = image_find(run);
        if (!image)
        {
            image = epd_app_alloc(sizeof(*image), EPD_APP_PSRAM);
            if (!image && required)
            { images_prune(true); image = epd_app_alloc(sizeof(*image), EPD_APP_PSRAM); }
            if (!image) { if (required) fail("当前页面图片内存不足"); return -1; }
            memset(image, 0, sizeof(*image));
            image->document = view.document; image->layout = view.layout; image->record = run->image - 1;
            image->width = run->width; image->height = run->height;
            image->next = view.images; view.images = image;
            image->request = document_image_request(view.document, image->record, image->width, image->height);
            if (!image->request) image->state = -1;
        }
        if (!image->state && image->request)
        {
            image->state = document_image_take(image->request, &image->bitmap);
            if (image->state)
            { document_image_cancel(image->request); image->request = NULL; }
        }
        if (image->state < 0 && required && !image->retried)
        {
            images_prune(true);
            image->retried = true;
            image->request = document_image_request(view.document, image->record, image->width, image->height);
            image->state = image->request ? 0 : -1;
        }
        if (image->state < 0)
        {
            if (required) fail("当前页面图片所需内存不足或缓存读取失败");
            return -1;
        }
        if (!image->state) ready = 0;
    }
    return ready;
}
static bool images_ready(const view_page_t *page)
{
    for (unsigned i = 0; i < page->count; ++i)
    {
        if (!page->runs[i].image) continue;
        view_image_t *image = image_find(&page->runs[i]);
        if (!image || image->state != 1) return false;
    }
    return true;
}
static void neighbours_sync(void)
{
    unsigned desired[2] = {view.wanted + 1, view.wanted ? view.wanted - 1 : UINT_MAX};
    if (view.direction < 0) { unsigned tmp = desired[0]; desired[0] = desired[1]; desired[1] = tmp; }
    for (unsigned i = 0; i < 2; ++i)
    {
        neighbour_t *n = &view.neighbours[i];
        if (n->layout == view.layout && n->number == desired[i]) continue;
        n->layout = view.layout; n->number = desired[i]; n->valid = n->unavailable = false;
        n->warm_run = n->warm_byte = 0;
        if (n->page) page_reset(n->page, (document_cursor_t){UINT32_MAX, 0});
    }
}
static bool neighbour_allocate(neighbour_t *n)
{
    if (n->unavailable) return false;
    if (!n->page)
    {
        n->page = epd_app_alloc(sizeof(*n->page), EPD_APP_PSRAM);
        if (n->page) page_reset(n->page, (document_cursor_t){UINT32_MAX, 0});
    }
    if (!n->page) { n->unavailable = true; return false; }
    if (!n->valid && !cursor_equal(n->page->start, view.pages[n->number]))
        page_reset(n->page, view.pages[n->number]);
    return true;
}
static bool page_runnable(const view_page_t *page)
{
    return !page->done && (page->cursor.record < view.progress.end || view.progress.complete);
}
static void restore_target(void)
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
    if (target_needed() && !view.display_pending)
        for (unsigned i = 0; i < 2; ++i)
        {
            neighbour_t *n = &view.neighbours[i];
            if (!n->valid || n->layout != view.layout || n->number != view.wanted ||
                !cursor_equal(n->page->start, view.pages[view.wanted])) continue;
            view_page_t *old = view.building; view.building = n->page; n->page = old;
            n->valid = false; n->layout = 0;
            view.display_pending = true; view.notified = false;
            ++view.reused;
            break;
        }
    neighbours_sync();
}
static void index_finish(void)
{
    if (!view.index->count && view.index->eof && view.count) { view.complete = true; return; }
    if (!reserve_pages(view.count + 2)) { fail("分页索引内存不足"); return; }
    unsigned number = view.count++;
    view.pages[view.count] = view.index->cursor;
    view.complete = view.index->eof;
    restore_target();
    if (view.index->collect)
    {
        if (!view.restoring && !view.link_target[0] && number == view.wanted)
        {
            view_page_t *old = view.building;
            view.building = view.index; view.index = old;
            view.display_pending = true; view.notified = false;
            ++view.reused;
        }
        else for (unsigned i = 0; i < 2; ++i)
        {
            neighbour_t *n = &view.neighbours[i];
            if (n->number != number || !neighbour_allocate(n)) continue;
            view_page_t *old = n->page;
            n->page = view.index; view.index = old; n->valid = true;
            n->warm_run = n->warm_byte = 0;
            break;
        }
    }
    page_reset(view.index, view.pages[view.count]);
    view.index->collect = view.restoring || view.count == view.wanted ||
                         view.count == view.wanted + 1 || view.count + 1 == view.wanted;
}
static bool link_process(void)
{
    if (!view.link_target[0]) return false;
    if (view.link_scan == view.progress.end)
    {
        if (view.progress.complete) { view.link_target[0] = 0; fail("未找到文档内部链接目标"); }
        return false;
    }
    document_record_t r;
    if (view.link_scan > view.progress.end || view.progress.end - view.link_scan < sizeof(r) ||
        !document_reader_record(view.aux_reader, view.link_scan, &r) ||
        r.length > view.progress.end - view.link_scan - sizeof(r))
    { fail("链接索引读取失败"); return false; }
    if (r.kind == DOCUMENT_ANCHOR && r.length <= DOCUMENT_TEXT_LIMIT)
    {
        char text[DOCUMENT_TEXT_LIMIT + 1];
        if (!document_reader_read(view.aux_reader, view.link_scan + sizeof(r), text, r.length))
        { fail("链接索引读取失败"); return false; }
        text[r.length] = 0;
        size_t id_length = strlen(text);
        bool definition = id_length + 1 < r.length && !text[id_length + 1];
        if (definition && (!strcmp(text, view.link_target) ||
            (view.link_target[0] == '#' && !strcmp(text, view.link_target + 1))))
        {
            view.anchor = (document_cursor_t){view.link_scan, 0};
            view.restoring = true; view.link_target[0] = 0;
            view.display_pending = view.notified = false;
            return true;
        }
    }
    view.link_scan += sizeof(r) + r.length;
    return true;
}
static bool compute_one(bool foreground)
{
    if (view.link_target[0] && link_process()) return true;
    if (view.failure[0]) return false;
    restore_target();
    if (foreground && target_needed() && !view.display_pending)
    {
        if (!cursor_equal(view.building->start, view.pages[view.wanted]))
            page_reset(view.building, view.pages[view.wanted]);
        if (view.building->done)
        { view.display_pending = true; view.notified = false; return true; }
        if (page_runnable(view.building))
        {
            int result = page_step(view.building, false, view.page_reader, &view.progress);
            if (view.building->done && !cursor_equal(view.building->cursor, view.pages[view.wanted + 1]))
                result = -1;
            if (result < 0)
            {
                if (view.from_cache)
                {
                    view.from_cache = view.complete = view.index_saved = false;
                    view.anchor = view.pages[view.wanted]; view.restoring = true;
                    view.count = view.toc_count = view.toc_scanned = 0;
                    view.pages[0] = (document_cursor_t){0, 0};
                    page_reset(view.index, view.pages[0]); page_reset(view.building, view.pages[0]);
                    for (unsigned i = 0; i < 2; ++i) view.neighbours[i].layout = 0;
                    images_cancel_pending();
                    return true;
                }
                fail("文档页面读取失败");
            }
            if (view.building->done) { view.display_pending = true; view.notified = false; }
            return result > 0;
        }
    }
    if (!view.restoring && !view.link_target[0] && view.wanted < view.count)
    {
        for (unsigned i = 0; i < 2; ++i)
        {
            neighbour_t *n = &view.neighbours[i];
            if (n->number == view.count && !view.complete && page_runnable(view.index)) break;
            if (n->number >= view.count || !neighbour_allocate(n)) continue;
            if (!n->valid && page_runnable(n->page))
            {
                int result = page_step(n->page, false, view.page_reader, &view.progress);
                if (result < 0) { n->unavailable = true; return true; }
                if (n->page->done)
                {
                    n->valid = cursor_equal(n->page->cursor, view.pages[n->number + 1]);
                    if (!n->valid) n->unavailable = true;
                }
                return result > 0;
            }
            if (n->valid && n->warm_run < n->page->count)
            {
                view_run_t *run = &n->page->runs[n->warm_run];
                if (run->image || n->warm_byte >= run->bytes)
                { ++n->warm_run; n->warm_byte = 0; return true; }
                uint32_t letter;
                n->warm_byte += utf8_letter(n->page->text + run->text + n->warm_byte,
                                            run->bytes - n->warm_byte, &letter);
                lv_draw_wait_for_finish();
                epd_app_font_prepare(view.fonts[font_slot(run->style)], letter, true);
                return true;
            }
        }
    }
    if (!view.complete && page_runnable(view.index))
    {
        int result = page_step(view.index, true, view.index_reader, &view.progress);
        if (result < 0) fail("文档排版失败");
        if (result > 0 && view.index->done) index_finish();
        return result > 0;
    }
    return false;
}
static bool target_runnable(void)
{
    if (view.failure[0]) return false;
    if (view.link_target[0]) return view.link_scan < view.progress.end || view.progress.complete;
    if (view.restoring)
    {
        if (view.count && (view.complete || cursor_before(view.anchor, view.pages[view.count]))) return true;
        return !view.complete && page_runnable(view.index);
    }
    if (view.wanted >= view.count) return !view.complete && page_runnable(view.index);
    if (target_needed() && !view.display_pending)
    {
        if (!cursor_equal(view.building->start, view.pages[view.wanted]) ||
            view.building->done || page_runnable(view.building)) return true;
        for (unsigned i = 0; i < 2; ++i)
            if (view.neighbours[i].valid && view.neighbours[i].layout == view.layout &&
                view.neighbours[i].number == view.wanted) return true;
    }
    return false;
}
static bool compute_runnable(bool foreground)
{
    if (view.failure[0]) return false;
    if ((foreground || view.restoring || view.link_target[0]) && target_runnable()) return true;
    if (!view.restoring && !view.link_target[0] && view.wanted < view.count)
        for (unsigned i = 0; i < 2; ++i)
        {
            neighbour_t *n = &view.neighbours[i];
            if (n->number < view.count && !n->unavailable &&
                (!n->page || (!n->valid && page_runnable(n->page)) ||
                 (n->valid && n->warm_run < n->page->count))) return true;
        }
    return !view.complete && page_runnable(view.index);
}
static void publish_state(bool foreground)
{
    if (view.failure[0])
    {
        if (strcmp(view.view.error, view.failure))
        { snprintf(view.view.error, sizeof(view.view.error), "%s", view.failure); ++view.view.revision; }
        view.view.indexing = false;
        return;
    }
    bool finished = view.complete && view.view.pages != view.count;
    view.view.indexing = !view.complete;
    if (view.complete) view.view.pages = view.count;
    if (foreground && view.display_pending && !view.notified && images_ready(view.building))
    { view.notified = true; ++view.view.revision; }
    if (finished && foreground && view.view.ready && view.display_layout == view.layout) save_position();
}
reader_work_t epd_reader_step(uint32_t budget_ms, bool foreground, bool commit)
{
    if (!view.document) return READER_WORK_IDLE;
    rt_tick_t started = rt_tick_get();
    rt_tick_t budget = rt_tick_from_millisecond(budget_ms > INT_MAX ? INT_MAX : (int)budget_ms);
    if (!budget) budget = 1;
    document_progress(view.document, &view.progress);
    if (view.progress.result)
    {
        document_status_t status;
        document_status(view.document, &status);
        fail(status.error[0] ? status.error : "文档读取失败");
    }
    index_load();
    bool present = foreground && commit && epd_reader_waiting();
    if (budget_ms && !view.failure[0])
        while ((!present || !view.display_pending) && compute_one(foreground))
            if (view.failure[0] || rt_tick_get() - started >= budget) break;
    if (!view.failure[0])
    {
        images_prune(false);
        if (view.display_pending) images_prepare(view.building, true);
        else if (view.view.ready && !view.root) images_prepare(view.display, true);
        if (!view.restoring && !view.link_target[0] && (!target_needed() ||
            (view.display_pending && images_ready(view.building))))
            for (unsigned i = 0; i < 2 && !view.failure[0]; ++i)
                if (view.neighbours[i].valid) images_prepare(view.neighbours[i].page, false);
        index_save();
    }
    if (view.failure[0]) images_cancel_pending();
    if (view.display_pending && !view.prepared_logged && !view.failure[0] && images_ready(view.building))
    {
        view.prepared_logged = true;
#if DOCUMENT_VIEW_TRACE
        rt_kprintf("[document] target-prepared: page=%u, %lu ms; blocks=%lu chars=%lu metrics=%lu layouts=%lu reused=%lu index-hit=%lu\n",
            view.wanted + 1, (unsigned long)((uint64_t)(rt_tick_get() - view.requested) * 1000 / RT_TICK_PER_SECOND),
            (unsigned long)view.blocks, (unsigned long)view.characters, (unsigned long)view.measurements,
            (unsigned long)view.layouts, (unsigned long)view.reused, (unsigned long)view.index_hits);
#endif
    }
    if (commit)
    {
        document_status_t status;
        document_status(view.document, &status);
        if (status.title[0]) snprintf(view.title, sizeof(view.title), "%s", status.title);
        publish_state(foreground);
    }
    if (foreground && target_runnable()) return READER_WORK_TARGET;
    return compute_runnable(foreground) ? READER_WORK_BACKGROUND : READER_WORK_IDLE;
}

static void link_clicked(lv_event_t *event)
{
    view_widget_t *widget = lv_event_get_user_data(event);
    if (widget->link && epd_app_refresh_done() && !epd_reader_waiting())
        epd_reader_follow_link(widget->link);
}
static view_widget_t *widget_take(bool image)
{
    for (view_widget_t *w = view.widgets; w; w = w->next)
        if (!w->used && w->image == image) { w->used = true; return w; }
    view_widget_t *w = epd_app_alloc(sizeof(*w), EPD_APP_PSRAM);
    if (!w) return NULL;
    memset(w, 0, sizeof(*w)); w->image = image; w->used = true;
    w->object = image ? lv_image_create(view.root) : lv_label_create(view.root);
    if (!w->object) { epd_app_free(w); return NULL; }
    lv_obj_add_flag(w->object, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(w->object, link_clicked, LV_EVENT_CLICKED, w);
    w->next = view.widgets; view.widgets = w;
    return w;
}
bool epd_reader_bind(lv_obj_t *body)
{
    if (!view.document || view.failure[0]) return false;
    if (!view.display_pending && view.root) return true;
    if (!view.display_pending && view.display_layout != view.layout) return false;
    if ((!view.display_pending && !view.view.ready) || (view.display_pending && !view.notified)) return false;
    view_page_t *page = view.display_pending ? view.building : view.display;
    if (!images_ready(page)) return false;
    if (!view.root)
    {
        view.root = lv_obj_create(body);
        if (!view.root) { fail("正文控件内存不足"); publish_state(true); return false; }
        epd_app_object_init(view.root);
        lv_obj_set_style_pad_all(view.root, 0, 0); lv_obj_set_style_border_width(view.root, 0, 0);
        lv_obj_set_style_bg_opa(view.root, LV_OPA_COVER, 0); lv_obj_set_style_bg_color(view.root, lv_color_white(), 0);
        lv_obj_remove_flag(view.root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(view.root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(view.root, root_deleted, LV_EVENT_DELETE, NULL);
    }
    for (view_widget_t *w = view.widgets; w; w = w->next) w->used = false;
    for (unsigned i = 0; i < page->count; ++i)
        if (!widget_take(page->runs[i].image != 0))
        { fail("正文控件内存不足"); publish_state(true); return false; }
    lv_draw_wait_for_finish();
    for (view_image_t *image = view.images; image; image = image->next) image->bound = false;
    for (view_widget_t *w = view.widgets; w; w = w->next) w->used = false;
    for (unsigned i = 0; i < page->count; ++i)
    {
        view_run_t *run = &page->runs[i];
        view_widget_t *w = widget_take(run->image != 0);
        lv_obj_t *object = w->object;
        w->link = run->link;
        if (run->image)
        {
            view_image_t *image = image_find(run);
            image->bound = true;
            lv_image_set_src(object, &image->bitmap.image);
        }
        else
        {
            lv_obj_set_style_text_font(object, font_for(run->style), 0);
            lv_obj_set_style_text_color(object, lv_color_black(), 0);
            lv_obj_set_style_text_line_space(object, 0, 0);
            lv_obj_set_style_text_letter_space(object, 0, 0);
            lv_label_set_long_mode(object, LV_LABEL_LONG_CLIP);
            lv_obj_set_size(object, run->width + 2, run->height);
            lv_label_set_text_static(object, page->text + run->text);
            lv_obj_set_style_text_decor(object, run->link ? LV_TEXT_DECOR_UNDERLINE : LV_TEXT_DECOR_NONE, 0);
        }
        lv_obj_set_pos(object, run->x, run->y);
        if (run->link) lv_obj_add_flag(object, LV_OBJ_FLAG_CLICKABLE);
        else lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
    for (view_widget_t *w = view.widgets; w; w = w->next)
        if (!w->used)
        {
            w->link = 0;
            lv_obj_add_flag(w->object, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(w->object, LV_OBJ_FLAG_CLICKABLE);
            if (w->image) lv_image_set_src(w->object, NULL);
            else lv_label_set_text_static(w->object, "");
        }
    lv_obj_set_size(view.root, width_available(), VIEW_HEIGHT);
    lv_obj_set_pos(body, view.view.margin, READER_TOP);
    lv_obj_set_size(body, width_available(), VIEW_HEIGHT);
    lv_obj_remove_flag(view.root, LV_OBJ_FLAG_HIDDEN);
    if (view.display_pending)
    {
        bool same_layout = view.display_layout == view.layout;
        view_page_t *old = view.display; view.display = view.building; view.building = old;
        view.display_layout = view.layout;
        view.view.page = view.wanted + 1;
        view.view.percent = view.progress.complete && view.progress.end ?
            (unsigned)((uint64_t)view.display->cursor.record * 100 / view.progress.end) : 0;
        if (!view.view.ready)
            rt_kprintf("[document] first-page-ready: page=%u, %lu ms\n", view.view.page,
                (unsigned long)((uint64_t)(rt_tick_get() - view.opened) * 1000 / RT_TICK_PER_SECOND));
        view.view.ready = true; view.display_pending = view.notified = false;
        /* Retain the former visible page as the reverse neighbour. */
        for (unsigned i = 0; i < 2; ++i)
        {
            neighbour_t *n = &view.neighbours[i];
            if (!same_layout || n->number >= view.count || !old->done || !old->collect ||
                !cursor_equal(old->start, view.pages[n->number]) ||
                !cursor_equal(old->cursor, view.pages[n->number + 1])) continue;
            if (!neighbour_allocate(n)) break;
            view.building = n->page; n->page = old; n->valid = true;
            n->warm_run = n->page->count; n->warm_byte = 0;
            break;
        }
        page_reset(view.building, (document_cursor_t){UINT32_MAX, 0});
        save_position();
    }
    images_prune(false);
    for (unsigned i = 0; i < 3; ++i) { document_font_free(view.retired_oblique[i]); view.retired_oblique[i] = NULL; }
    for (unsigned i = 0; i < 6; ++i) { epd_app_font_destroy(view.retired[i]); view.retired[i] = NULL; }
    return true;
}

bool epd_reader_save(void) { save_position(); return view.view.saved; }
bool epd_reader_reflow(void)
{
    if (!view.document) return false;
    save_position();
    view.anchor = view.view.ready ? view.display->start : view.anchor;
    document_status_t status;
    document_status(view.document, &status);
    if (status.error[0])
    {
        char error[96] = "文档不可用，请重新打开";
        document_t *replacement = view.reopen ? view.reopen(view.user, error, sizeof(error)) : NULL;
        if (!replacement) { fail(error); return false; }
        images_cancel_pending();
        readers_destroy();
        document_close(view.document);
        view.document = replacement;
        if (!readers_create()) { fail("文档读取游标内存不足"); publish_state(true); return false; }
        document_service_process();
        view.toc_count = view.toc_scanned = 0;
        view.opened = rt_tick_get();
    }
    if (!prepare_fonts()) { fail("字体内存不足"); publish_state(true); return false; }
    view.view.error[0] = 0;
    view.failure[0] = 0;
    view.link_target[0] = 0;
    view.link_scan = 0;
    view.count = 0; view.complete = false; view.restoring = true; view.display_pending = view.notified = false;
    view.index_loaded = view.index_saved = view.from_cache = false;
    view.toc_count = view.toc_scanned = 0;
    for (unsigned i = 0; i < 2; ++i) { view.neighbours[i].valid = false; view.neighbours[i].layout = 0; }
    page_reset(view.index, (document_cursor_t){0, 0});
    page_reset(view.building, (document_cursor_t){0, 0});
    images_cancel_pending();
    images_prune(true);
    view.view.pages = 0; view.view.indexing = true;
    view.requested = rt_tick_get(); view.prepared_logged = false;
    ++view.view.revision;
    return true;
}
bool epd_reader_settings_changed(void)
{
    for (unsigned i = UI_SETTING_FONT; i < UI_SETTING_COUNT; ++i)
        if (view.settings[i - UI_SETTING_FONT] != ui_settings_index(i)) return true;
    return false;
}
void epd_reader_seek(unsigned page)
{
    if (!page || !view.document || view.failure[0]) return;
    if (view.view.pages && page > view.view.pages) page = view.view.pages;
    if (page > READER_PAGE_MAX) page = READER_PAGE_MAX;
    if (!view.restoring && !view.link_target[0] && view.wanted == page - 1) return;
    view.requested = rt_tick_get(); view.prepared_logged = false;
    if (page - 1 != view.wanted) view.direction = page - 1 > view.wanted ? 1 : -1;
    view.restoring = false; view.wanted = page - 1; view.link_target[0] = 0;
    view.display_pending = view.notified = false;
    images_cancel_pending();
    page_reset(view.building, view.wanted < view.count ? view.pages[view.wanted] : (document_cursor_t){0, 0});
    restore_target();
    images_prune(false);
}
void epd_reader_cancel_pending(void)
{
    if (!view.view.ready || view.failure[0]) return;
    if (view.display_layout != view.layout)
    {
        /* The visible page number belongs to the previous layout. Resolve its
         * original text cursor again, even if a new page is already prepared. */
        view.anchor = view.display->start;
        view.restoring = true;
        view.link_target[0] = 0; view.link_scan = 0;
        view.display_pending = view.notified = false;
        images_cancel_pending();
        images_prune(true);
        return;
    }
    epd_reader_seek(view.view.page);
}
bool epd_reader_waiting(void)
{
    return !view.failure[0] && (!view.view.ready || view.restoring || view.link_target[0] ||
        view.wanted + 1 != view.view.page || view.display_pending ||
        view.display_layout != view.layout);
}
unsigned epd_reader_toc_count(void) { return view.toc_count; }
bool epd_reader_toc_title(unsigned index, char *title, size_t size, unsigned *level)
{
    if (index >= view.toc_count) return false;
    document_record_t r;
    char data[DOCUMENT_TEXT_LIMIT + 1];
    if (!document_reader_record(view.aux_reader, view.toc[index].record, &r) || r.kind != DOCUMENT_ANCHOR ||
        r.style != view.toc[index].level || r.length > DOCUMENT_TEXT_LIMIT ||
        !document_reader_read(view.aux_reader, view.toc[index].record + sizeof(r), data, r.length)) return false;
    data[r.length] = 0;
    size_t start = strlen(data) + 1;
    if (start >= r.length || !data[start] || !memchr(data + start, 0, r.length - start)) return false;
    snprintf(title, size, "%s", data + start);
    if (level) *level = view.toc[index].level;
    return true;
}
bool epd_reader_toc_seek(unsigned index)
{
    if (index >= view.toc_count) return false;
    document_record_t r;
    if (!document_reader_record(view.aux_reader, view.toc[index].record, &r) || r.kind != DOCUMENT_ANCHOR ||
        r.style != view.toc[index].level || r.length > DOCUMENT_TEXT_LIMIT ||
        !document_reader_read(view.aux_reader, view.toc[index].record + sizeof(r), view.link_target, r.length)) return false;
    view.link_target[r.length] = 0;
    view.link_scan = 0;
    view.requested = rt_tick_get(); view.prepared_logged = false;
    view.display_pending = view.notified = false;
    images_cancel_pending();
    images_prune(true);
    return true;
}
bool epd_reader_follow_link(uint32_t record)
{
    document_record_t r;
    if (!record || !document_reader_record(view.aux_reader, record - 1, &r) || r.kind != DOCUMENT_LINK || r.length > DOCUMENT_TEXT_LIMIT ||
        !document_reader_read(view.aux_reader, record - 1 + sizeof(r), view.link_target, r.length)) return false;
    view.link_target[r.length] = 0;
    if (strstr(view.link_target, "://") || !strncmp(view.link_target, "mailto:", 7))
    { view.link_target[0] = 0; return false; }
    view.link_scan = 0;
    view.requested = rt_tick_get(); view.prepared_logged = false;
    view.display_pending = view.notified = false;
    images_cancel_pending();
    images_prune(true);
    return true;
}

void epd_reader_seek_cursor(document_cursor_t cursor)
{
    if (!view.document || view.failure[0]) return;
    view.anchor = cursor; view.restoring = true;
    view.link_target[0] = 0; view.link_scan = 0;
    view.display_pending = view.notified = false;
    view.requested = rt_tick_get(); view.prepared_logged = false;
    images_cancel_pending();
    page_reset(view.building, (document_cursor_t){0, 0});
    restore_target();
    images_prune(true);
}

void epd_reader_collect(void) { document_service_process(); }
void epd_reader_set_error(const char *message)
{ snprintf(view.view.error, sizeof(view.view.error), "%s", message); }
