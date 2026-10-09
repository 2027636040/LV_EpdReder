#include "ui_reader.h"
#include "storage.h"
#include "storage_file.h"
#include "ui_bookshelf_data.h"
#include "ui_settings.h"
#include "platform/epd_app.h"
#include "reader.h"
#include "document/document_view.h"
#include "src/misc/lv_text_private.h"
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

#define TEXT_CAPACITY 16384u
#define WINDOW_CAPACITY 4096u
#define STREAM_CAPACITY (2 * WINDOW_CAPACITY)
#define METRIC_CACHE_SIZE 1024u
#define INDEX_BUDGET_MS 8
#define PREPARE_BUDGET_MS 8
#define PAGE_CACHE_COUNT 3
#define CHARACTER_SET_BYTES (0x110000u / 8u)
#define MAX_PAGES UI_READER_PAGE_MAX
#define INDEX_MAGIC 0x45504932u

typedef struct
{
    uint32_t magic, size, modified, layout, count, checksum;
    char path[BOOKSHELF_PATH_MAX];
} index_header_t;

typedef struct
{
    char *text;
    uint32_t *mapping;
    unsigned head, tail;
    uint32_t source;
} text_stream_t;

typedef struct
{
    uint32_t letter;
    uint16_t width;
    bool valid;
} metric_t;

typedef struct
{
    char *text;
    uint32_t end;
    unsigned page, bytes, lines, warmed;
    bool valid;
} prepared_page_t;

static struct
{
    ui_reader_view_t view;
    reader_info_t file;
    lv_font_t *font, *fallback;
    char *text, *scratch;
    text_stream_t index_stream, display_stream, prepare_stream;
    prepared_page_t prepared[PAGE_CACHE_COUNT];
    int building;
    unsigned prepare_page, ahead_head, ahead_tail;
    uint32_t ahead_source;
    char *ahead_text;
    uint8_t *characters;
    unsigned unique_characters, prepared_characters;
    bool scan_complete;
    bool prepare_blocked;
    lv_font_t measure_font;
    metric_t *metrics;
    uint32_t *offsets;
    unsigned index_line, index_bytes;
    uint32_t index_end;
    unsigned capacity, count, book, wanted;
    uint32_t layout, anchor;
    unsigned settings[UI_SETTING_COUNT - UI_SETTING_FONT];
    bool opened, complete, restoring;
} reader;

static uint32_t offsets_hash(const uint32_t *data, unsigned count)
{
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < count; ++i) h = (h ^ data[i]) * 16777619u;
    return h;
}

static bool reserve(unsigned count)
{
    if (count > MAX_PAGES + 1 || count > SIZE_MAX / sizeof(*reader.offsets)) return false;
    if (count <= reader.capacity) return true;
    unsigned capacity = reader.capacity ? reader.capacity * 2 : 256;
    if (capacity < count) capacity = count;
    if (capacity > SIZE_MAX / sizeof(*reader.offsets)) capacity = count;
    uint32_t *data = epd_app_realloc(reader.offsets, capacity * sizeof(*data), EPD_APP_PSRAM);
    if (!data) return false;
    reader.offsets = data;
    reader.capacity = capacity;
    return true;
}

static void fail(const char *message)
{
    snprintf(reader.view.error, sizeof(reader.view.error), "%s", message);
    reader.view.indexing = false;
    ++reader.view.revision;
}

/* This font is used only by the line breaker, never to draw glyphs. */
static bool metric_descriptor(const lv_font_t *font, lv_font_glyph_dsc_t *glyph,
                               uint32_t letter, uint32_t next)
{
    const lv_font_t *source = font->user_data;
    metric_t *entry = &reader.metrics[(letter * 2654435761u) >> 22];
    if (source->kerning == LV_FONT_KERNING_NONE && entry->valid && entry->letter == letter)
    {
        glyph->adv_w = entry->width;
        return true;
    }
    lv_font_glyph_dsc_t measured;
    bool found = lv_font_get_glyph_dsc(source, &measured, letter, next);
    glyph->adv_w = measured.adv_w;
    /* Do not retain a failed lookup or temporary placeholder as a real width. */
    if (found && !measured.is_placeholder && source->kerning == LV_FONT_KERNING_NONE)
    {
        entry->letter = letter;
        entry->width = measured.adv_w;
        entry->valid = true;
    }
    return true;
}

static bool stream_create(text_stream_t *stream)
{
    stream->text = epd_app_alloc(STREAM_CAPACITY, EPD_APP_PSRAM);
    stream->mapping = epd_app_alloc(STREAM_CAPACITY * sizeof(uint32_t), EPD_APP_PSRAM);
    return stream->text && stream->mapping;
}

static void stream_reset(text_stream_t *stream, uint32_t start)
{
    stream->head = stream->tail = 0;
    stream->source = start;
    stream->text[0] = '\0';
    stream->mapping[0] = start;
}

static void stream_destroy(text_stream_t *stream)
{
    epd_app_free(stream->text);
    epd_app_free(stream->mapping);
}

static unsigned page_lines(void)
{
    unsigned step = reader.font->line_height + reader.view.line_space;
    return (UI_READER_BOTTOM - UI_READER_TOP + reader.view.line_space) / step;
}

/* Retain decoded look-ahead and original byte offsets across pages. Compact
 * only when a refill needs space; display seeks use a separate stream. */
static int measure_line(text_stream_t *stream, char *output, unsigned *copied, uint32_t *end)
{
    unsigned available = stream->tail - stream->head;
    while (available < WINDOW_CAPACITY - 8 && stream->source < reader.file.file_size)
    {
        unsigned room = WINDOW_CAPACITY - available;
        if (STREAM_CAPACITY - stream->tail < room)
        {
            memmove(stream->text, stream->text + stream->head, available + 1);
            memmove(stream->mapping, stream->mapping + stream->head,
                    (available + 1) * sizeof(uint32_t));
            stream->head = 0;
            stream->tail = available;
        }
        uint32_t next = stream->source;
        int n = reader_read_mapped(stream->source, stream->text + stream->tail, room,
                                   stream->mapping + stream->tail, &next);
        if (n < 0 || next <= stream->source) return -RT_EIO;
        stream->source = next;
        stream->tail += n;
        available += n;
    }
    if (!available) return 0;
    lv_text_attributes_t attr;
    lv_text_attributes_init(&attr);
    attr.max_width = 684 - 2 * reader.view.margin;
    attr.line_space = reader.view.line_space;
    const char *text = stream->text + stream->head;
    unsigned n = lv_text_get_next_line(text, available, &reader.measure_font, NULL, &attr);
    if (!n || n > available || *copied + n + 1 >= TEXT_CAPACITY) return -RT_ERROR;
    if (output) memcpy(output + *copied, text, n);
    *copied += n;
    /* Freeze soft breaks to retain the last line's English word look-ahead. */
    if (text[n - 1] != '\n')
    {
        if (output) output[*copied] = '\n';
        ++*copied;
    }
    stream->head += n;
    *end = stream->mapping[stream->head];
    return 1;
}

static int page_measure(uint32_t start, char *output, uint32_t *end)
{
    text_stream_t *stream = &reader.display_stream;
    if (stream->mapping[stream->head] != start) stream_reset(stream, start);
    unsigned copied = 0;
    *end = start;
    for (unsigned line = 0; line < page_lines(); ++line)
    {
        int result = measure_line(stream, output, &copied, end);
        if (result < 0) return result;
        if (!result) break;
    }
    output[copied] = '\0';
    return (int)copied;
}

static prepared_page_t *prepared_find(unsigned page)
{
    for (unsigned i = 0; i < PAGE_CACHE_COUNT; ++i)
        if (reader.prepared[i].valid && reader.prepared[i].page == page) return &reader.prepared[i];
    return NULL;
}

static prepared_page_t *prepared_slot(unsigned page)
{
    prepared_page_t *slot = &reader.prepared[0];
    unsigned distance = 0;
    for (unsigned i = 0; i < PAGE_CACHE_COUNT; ++i)
    {
        prepared_page_t *p = &reader.prepared[i];
        if (!p->valid) return p;
        unsigned d = p->page > page ? p->page - page : page - p->page;
        if (d > distance) { distance = d; slot = p; }
    }
    return slot;
}

static uint32_t next_letter(const char *text, unsigned *offset)
{
    const unsigned char *p = (const unsigned char *)text + *offset;
    uint32_t letter = *p++;
    unsigned rest = letter < 0x80 ? 0 : letter < 0xe0 ? 1 : letter < 0xf0 ? 2 : 3;
    if (rest) letter &= (1u << (6 - rest)) - 1;
    *offset += rest + 1;
    while (rest--) letter = (letter << 6) | (*p++ & 0x3f);
    return letter;
}

static bool prepare_neighbour(unsigned page)
{
    if (page >= reader.count) return false;
    prepared_page_t *p = prepared_find(page);
    if (p)
    {
        if (p->warmed >= p->bytes) return false;
        unsigned next = p->warmed;
        uint32_t letter = next_letter(p->text, &next);
        /* A speculative allocation failure must not starve the rest of the
         * page/book. Actual drawing can retry through the original callback. */
        epd_app_font_prepare(reader.font, letter, true);
        p->warmed = next;
        return true;
    }
    if (reader.building < 0 || reader.prepared[reader.building].page != page)
    {
        p = prepared_slot(reader.view.page - 1);
        reader.building = p - reader.prepared;
        p->page = page;
        p->bytes = p->lines = p->warmed = 0;
        p->end = reader.offsets[page];
        p->valid = false;
        stream_reset(&reader.prepare_stream, p->end);
    }
    else p = &reader.prepared[reader.building];
    int result = measure_line(&reader.prepare_stream, p->text, &p->bytes, &p->end);
    if (result < 0) { reader.prepare_blocked = true; return true; }
    if (!result || ++p->lines >= page_lines() || p->end >= reader.file.file_size)
    {
        p->text[p->bytes] = 0;
        p->valid = p->end == reader.offsets[page + 1];
        reader.building = -1;
        if (!p->valid) reader.prepare_blocked = true;
    }
    return true;
}

void ui_reader_prepare_process(void)
{
    if (document_view_active()) { document_view_prepare(); return; }
    if (!reader.opened || ui_reader_waiting() || reader.view.error[0]) return;
    reader_info_t current;
    if (!reader_get_info(&current) || strcmp(current.path, reader.file.path)) return;
    if (reader.prepare_page != reader.view.page)
    {
        reader.prepare_page = reader.view.page;
        reader.building = -1;
        reader.prepare_blocked = false;
        for (unsigned i = 0; i < PAGE_CACHE_COUNT; ++i) reader.prepared[i].warmed = 0;
    }
    if (reader.prepare_blocked) return;
    /* Finish outstanding LVGL/EPIC tasks, not the LCD panel's waveform. */
    lv_draw_wait_for_finish();
    rt_tick_t started = rt_tick_get();
    do
    {
        if (prepare_neighbour(reader.view.page)) continue;
        if (reader.view.page > 1 && prepare_neighbour(reader.view.page - 2)) continue;
        if (reader.ahead_head == reader.ahead_tail)
        {
            if (reader.ahead_source >= reader.file.file_size)
            {
                if (!reader.scan_complete)
                {
                    reader.scan_complete = true;
                    LV_LOG_USER("book characters: unique=%u, prepared=%u",
                                reader.unique_characters, reader.prepared_characters);
                }
                break;
            }
            uint32_t next = reader.ahead_source;
            int n = reader_read_text(next, reader.ahead_text, STREAM_CAPACITY, &next);
            if (n <= 0) { reader.prepare_blocked = true; break; }
            reader.ahead_head = 0;
            reader.ahead_tail = n;
            reader.ahead_source = next;
        }
        unsigned next = reader.ahead_head;
        uint32_t letter = next_letter(reader.ahead_text, &next);
        reader.ahead_head = next;
        if (letter < 0x20 || letter == 0x7f || letter >= 0x110000u) continue;
        uint8_t bit = (uint8_t)(1u << (letter & 7u));
        if (!(reader.characters[letter >> 3] & bit))
        {
            reader.characters[letter >> 3] |= bit;
            ++reader.unique_characters;
            /* Always visit the complete book for descriptor preparation. When
             * pixels reach their budget, keep scanning without evicting nearby
             * glyphs. The scan cursor is independent of page navigation. */
            if (epd_app_font_prepare(reader.font, letter, false)) ++reader.prepared_characters;
        }
    } while (!reader.prepare_blocked &&
             (rt_tick_t)(rt_tick_get() - started) < rt_tick_from_millisecond(PREPARE_BUDGET_MS));
}

static bool index_load(void)
{
    char path[64];
    index_header_t header;
    const ui_bookshelf_book_t *book = ui_bookshelf_book(reader.book);
    ui_bookshelf_cache_path(reader.book, path, sizeof(path), "idx");
    storage_lock();
    int fd = storage_file_recover(path) ? open(path, O_RDONLY) : -1;
    if (fd < 0) { storage_unlock(); return false; }
    bool ok = read(fd, &header, sizeof(header)) == sizeof(header) &&
        header.magic == INDEX_MAGIC && header.size == reader.file.file_size &&
        header.modified == book->file.modified && header.layout == reader.layout &&
        memchr(header.path, 0, sizeof(header.path)) &&
        !strcmp(storage_relative_path(header.path), storage_relative_path(book->file.path)) &&
        header.count > 0 && header.count <= MAX_PAGES && reserve(header.count + 1);
    if (ok)
    {
        unsigned bytes = (header.count + 1) * sizeof(uint32_t);
        ok = read(fd, reader.offsets, bytes) == bytes &&
             offsets_hash(reader.offsets, header.count + 1) == header.checksum &&
             reader.offsets[0] == reader.file.data_start &&
             reader.offsets[header.count] == reader.file.file_size;
        for (unsigned i = 1; ok && i <= header.count; ++i)
            if (reader.offsets[i] <= reader.offsets[i - 1] && reader.file.file_size != reader.file.data_start)
                ok = false;
    }
    close(fd);
    storage_unlock();
    if (ok) { reader.count = header.count; reader.complete = true; }
    return ok;
}

static void index_save(void)
{
    char path[64];
    index_header_t header;
    memset(&header, 0, sizeof(header));
    const ui_bookshelf_book_t *book = ui_bookshelf_book(reader.book);
    if (!storage_app_available("books") || !storage_path_available(book->file.path)) return;
    header.magic = INDEX_MAGIC;
    header.size = reader.file.file_size;
    header.modified = book->file.modified;
    header.layout = reader.layout;
    header.count = reader.count;
    header.checksum = offsets_hash(reader.offsets, reader.count + 1);
    memcpy(header.path, book->file.path, sizeof(header.path));
    char directory[64];
    ui_bookshelf_cache_path(reader.book, directory, sizeof(directory), "idx");
    char *separator = strrchr(directory, '/');
    if (separator) { *separator = 0; if (!storage_mkdirs(directory)) return; }
    ui_bookshelf_cache_path(reader.book, path, sizeof(path), "idx");
    char temporary[72];
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    storage_lock();
    int fd = storage_app_available("books") ? open(temporary, O_CREAT | O_TRUNC | O_WRONLY, 0) : -1;
    if (fd < 0) { storage_unlock(); return; }
    unsigned bytes = (reader.count + 1) * sizeof(uint32_t);
    bool ok = write(fd, &header, sizeof(header)) == sizeof(header) &&
              write(fd, reader.offsets, bytes) == bytes && fsync(fd) == 0;
    if (close(fd)) ok = false;
    if (ok) ok = storage_app_available("books") && storage_file_commit(temporary, path);
    if (!ok && storage_app_available("books")) unlink(temporary);
    storage_unlock();
}

static void position_save(void)
{
    if (!reader.view.ready) return;
    ui_reading_position_t position = {0};
    position.offset = reader.offsets[reader.view.page - 1];
    position.current_page = reader.view.page;
    position.total_pages = reader.complete ? reader.count : 0;
    position.layout = reader.layout;
    position.progress = reader.view.percent;
    const ui_reading_position_t *saved = &ui_bookshelf_book(reader.book)->position;
    if (reader.view.saved && position.offset == saved->offset &&
        position.current_page == saved->current_page && position.total_pages == saved->total_pages &&
        position.layout == saved->layout && position.progress == saved->progress) return;
    time_t now = time(NULL);
    if (now >= 1704067200) position.last_read = (uint32_t)now;
    reader.view.saved = ui_bookshelf_save(reader.book, &position);
    reader_set_position(position.offset);
    epd_app_set_recent_reading(reader.view.title, position.progress);
}

static void show_page(void)
{
    uint32_t start = reader.offsets[reader.wanted], end = start;
    prepared_page_t *cached = prepared_find(reader.wanted);
    int n;
    if (cached)
    {
        n = cached->bytes;
        end = cached->end;
        memcpy(reader.scratch, cached->text, n + 1);
    }
    else n = page_measure(start, reader.scratch, &end);
    if (n < 0 || end != reader.offsets[reader.wanted + 1])
    {
        fail("读取失败，请检查存储后重新打开书籍");
        return;
    }
    memcpy(reader.text, reader.scratch, n + 1);
    if (!cached)
    {
        cached = prepared_slot(reader.wanted);
        cached->page = reader.wanted;
        cached->bytes = n;
        cached->end = end;
        cached->warmed = 0;
        cached->valid = true;
        memcpy(cached->text, reader.scratch, n + 1);
    }
    reader.building = -1;
    reader.view.page = reader.wanted + 1;
    reader.view.ready = true;
    uint32_t size = reader.file.file_size - reader.file.data_start;
    reader.view.percent = !size ? 0 : end == reader.file.file_size ? 100 :
        (unsigned)((uint64_t)(start - reader.file.data_start) * 100 / size);
    position_save();
    ++reader.view.revision;
}

static bool layout_prepare(uint32_t anchor)
{
    reader.prepare_page = 0;
    reader.building = -1;
    for (unsigned i = 0; i < PAGE_CACHE_COUNT; ++i) reader.prepared[i].valid = false;
    static const unsigned sizes[] = {16, 18, 20, 22, 24, 28, 32, 36};
    static const unsigned margins[] = {4, 8, 12, 16, 20};
    uint32_t font_id;
    lv_font_t *fallback;
    lv_font_t *font = epd_app_font_create(ui_settings_index(UI_SETTING_FONT),
                                          sizes[ui_settings_index(UI_SETTING_FONT_SIZE)],
                                          ui_settings_index(UI_SETTING_FONT_WEIGHT), &font_id, &fallback);
    if (!font) { fail("字体内存不足，无法打开书籍"); return false; }
    unsigned encoding = ui_settings_index(UI_SETTING_ENCODING);
    if (reader_set_encoding(encoding) != RT_EOK)
    {
        epd_app_font_destroy(font);
        epd_app_font_destroy(fallback);
        fail("文件编码设置失败");
        return false;
    }
    epd_app_font_destroy(reader.font);
    epd_app_font_destroy(reader.fallback);
    reader.font = font;
    reader.fallback = fallback;
    reader.view.font = font;
    reader.ahead_head = reader.ahead_tail = 0;
    reader.ahead_source = reader.file.data_start;
    reader.unique_characters = reader.prepared_characters = 0;
    reader.scan_complete = reader.prepare_blocked = false;
    memset(reader.characters, 0, CHARACTER_SET_BYTES);
    memset(reader.metrics, 0, METRIC_CACHE_SIZE * sizeof(*reader.metrics));
    memset(&reader.measure_font, 0, sizeof(reader.measure_font));
    reader.measure_font.get_glyph_dsc = metric_descriptor;
    reader.measure_font.user_data = font;
    reader.measure_font.line_height = font->line_height;
    reader.measure_font.kerning = font->kerning;
    reader.view.margin = 24 + margins[ui_settings_index(UI_SETTING_MARGIN)];
    reader.view.line_space = font->line_height * ui_settings_index(UI_SETTING_LINE_SPACING) / 5;
    for (unsigned i = UI_SETTING_FONT; i < UI_SETTING_COUNT; ++i)
        reader.settings[i - UI_SETTING_FONT] = ui_settings_index(i);
    /* Bump the seed when pagination or normalization rules change. */
    uint32_t layout[] = {5, font_id, reader.view.margin, reader.view.line_space, encoding,
                         UI_READER_BOTTOM - UI_READER_TOP, font->line_height};
    reader.layout = offsets_hash(layout, sizeof(layout) / sizeof(layout[0]));
    reader.anchor = anchor < reader.file.data_start ? reader.file.data_start : anchor;
    if (reader.anchor > reader.file.file_size) reader.anchor = reader.file.data_start;
    reader.restoring = true;
    reader.count = 0;
    reader.index_line = reader.index_bytes = 0;
    reader.index_end = reader.file.data_start;
    stream_reset(&reader.index_stream, reader.file.data_start);
    stream_reset(&reader.display_stream, reader.file.data_start);
    reader.complete = false;
    reader.view.ready = false;
    reader.view.error[0] = '\0';
    reader.view.indexing = true;
    ++reader.view.revision;
    if (!index_load())
    {
        if (!reserve(2)) { fail("分页内存不足"); return false; }
        reader.offsets[0] = reader.file.data_start;
    }
    return true;
}

void ui_reader_close(void)
{
    if (document_view_active()) document_view_close();
    if (reader.opened) ui_reader_save();
    reader_close();
    epd_app_font_destroy(reader.font);
    epd_app_font_destroy(reader.fallback);
    epd_app_free(reader.text); epd_app_free(reader.scratch);
    stream_destroy(&reader.index_stream);
    stream_destroy(&reader.display_stream);
    stream_destroy(&reader.prepare_stream);
    for (unsigned i = 0; i < PAGE_CACHE_COUNT; ++i) epd_app_free(reader.prepared[i].text);
    epd_app_free(reader.ahead_text);
    epd_app_free(reader.characters);
    epd_app_free(reader.metrics); epd_app_free(reader.offsets);
    memset(&reader, 0, sizeof(reader));
}

bool ui_reader_open(unsigned book_index)
{
    ui_reader_close();
    const ui_bookshelf_book_t *book = ui_bookshelf_book(book_index);
    if (book && book->file.format != DOCUMENT_TXT) return document_view_open(book_index);
    if (!book || !storage_path_available(book->file.path) || reader_open(book->file.path) != RT_EOK) return false;
    reader.book = book_index;
    reader.opened = true;
    reader_get_info(&reader.file);
    reader.view.title = book->file.name;
    reader.text = epd_app_alloc(TEXT_CAPACITY, EPD_APP_PSRAM);
    reader.scratch = epd_app_alloc(TEXT_CAPACITY, EPD_APP_PSRAM);
    reader.ahead_text = epd_app_alloc(STREAM_CAPACITY, EPD_APP_PSRAM);
    reader.characters = epd_app_alloc(CHARACTER_SET_BYTES, EPD_APP_PSRAM);
    for (unsigned i = 0; i < PAGE_CACHE_COUNT; ++i)
    {
        reader.prepared[i].text = epd_app_alloc(TEXT_CAPACITY, EPD_APP_PSRAM);
        if (!reader.prepared[i].text) { ui_reader_close(); return false; }
    }
    reader.metrics = epd_app_alloc(METRIC_CACHE_SIZE * sizeof(*reader.metrics), EPD_APP_PSRAM);
    if (!reader.text || !reader.scratch || !reader.ahead_text || !reader.metrics || !reader.characters ||
        !stream_create(&reader.index_stream) || !stream_create(&reader.display_stream) ||
        !stream_create(&reader.prepare_stream))
    { ui_reader_close(); return false; }
    reader.text[0] = '\0';
    reader.view.text = reader.text;
    if (!layout_prepare(book->position.offset)) { ui_reader_close(); return false; }
    /* The page is activated only after navigation has reached the reader. */
    ui_reader_index_process();
    return true;
}

bool ui_reader_reflow(void)
{
    if (document_view_active()) return document_view_reflow();
    if (!reader.opened) return false;
    uint32_t anchor = reader.view.ready ? reader.offsets[reader.view.page - 1] : reader.anchor;
    position_save();
    return layout_prepare(anchor);
}

static void index_process(bool foreground)
{
    if (!reader.opened || reader.view.error[0]) return;
    reader_info_t current;
    if (!reader_get_info(&current) || strcmp(current.path, reader.file.path))
    {
        fail("书籍已被关闭或切换，请返回书架重开");
        return;
    }
    rt_tick_t started = rt_tick_get();
    rt_tick_t budget = rt_tick_from_millisecond(INDEX_BUDGET_MS);
    if (!budget) budget = 1;
    while (!reader.complete)
    {
        /* Present a requested page before spending time on later pages. */
        if (foreground && reader.count &&
            (reader.restoring ? reader.anchor < reader.offsets[reader.count] :
                               reader.wanted < reader.count && ui_reader_waiting())) break;
        uint32_t start = reader.offsets[reader.count];
        int n = measure_line(&reader.index_stream, NULL, &reader.index_bytes, &reader.index_end);
        if (n < 0) { fail("读取失败，请检查存储后重新打开书籍"); return; }
        ++reader.index_line;
        if (!n || reader.index_line >= page_lines() || reader.index_end >= reader.file.file_size)
        {
            uint32_t end = reader.index_end;
            if (end <= start && start < reader.file.file_size)
            { fail("读取失败，请检查存储后重新打开书籍"); return; }
            if (!reserve(reader.count + 2)) { fail("分页内存不足，无法继续排版"); return; }
            reader.offsets[++reader.count] = end;
            reader.index_line = reader.index_bytes = 0;
            reader.complete = end >= reader.file.file_size;
            if (reader.complete) index_save();
        }
        /* A line (including a storage read) is the smallest scheduling unit. */
        if ((rt_tick_t)(rt_tick_get() - started) >= budget) break;
    }
    reader.view.indexing = !reader.complete;
    reader.view.pages = reader.complete ? reader.count : 0;
}

void ui_reader_index_process(void)
{
    if (document_view_active()) { document_view_process(false); return; }
    index_process(false);
}

void ui_reader_cancel_pending(void)
{
    if (document_view_active()) { document_view_cancel_pending(); return; }
    if (reader.view.ready)
    {
        reader.wanted = reader.view.page - 1;
    }
}

void ui_reader_process(void)
{
    if (document_view_active()) { document_view_process(true); return; }
    index_process(true);
    if (!reader.opened || reader.view.error[0]) return;
    if (reader.restoring)
    {
        if (reader.complete || reader.anchor < reader.offsets[reader.count])
        {
            unsigned lo = 0, hi = reader.count;
            while (lo + 1 < hi)
            {
                unsigned mid = lo + (hi - lo) / 2;
                if (reader.offsets[mid] <= reader.anchor) lo = mid;
                else hi = mid;
            }
            reader.wanted = lo;
            reader.restoring = false;
        }
    }
    if (!reader.restoring)
    {
        if (reader.complete && reader.wanted >= reader.count) reader.wanted = reader.count - 1;
        if (reader.wanted < reader.count && (!reader.view.ready || reader.view.page != reader.wanted + 1))
            show_page();
    }
}

void ui_reader_seek(unsigned page)
{
    if (document_view_active()) { document_view_seek(page); return; }
    if (!reader.view.ready || reader.view.error[0] || !page) return;
    if (reader.complete && page > reader.count) page = reader.count;
    if (page > MAX_PAGES) page = MAX_PAGES;
    reader.wanted = page - 1;
}

void ui_reader_turn(int direction)
{
    if (document_view_active())
    {
        int next = (int)document_view_get()->page + direction;
        document_view_seek(next > 0 ? (unsigned)next : 1);
        return;
    }
    if (!reader.view.ready) return;
    int next = (int)reader.view.page + direction;
    if (next < 1) next = 1;
    ui_reader_seek((unsigned)next);
}

const ui_reader_view_t *ui_reader_view(void) { return document_view_active() ? document_view_get() : &reader.view; }
bool ui_reader_save(void)
{
    if (document_view_active()) return document_view_save();
    position_save();
    bool flushed = ui_bookshelf_flush();
    if (!flushed || (reader.view.ready && !reader.view.saved))
    {
        reader.view.saved = false;
        rt_kprintf("books: pending reading position could not be flushed\n");
        return false;
    }
    return true;
}
bool ui_reader_waiting(void)
{
    if (document_view_active()) return document_view_waiting();
    return !reader.view.error[0] && (!reader.view.ready || reader.restoring ||
                                    reader.wanted + 1 != reader.view.page);
}

bool ui_reader_settings_changed(void)
{
    if (document_view_active()) return document_view_settings_changed();
    for (unsigned i = UI_SETTING_FONT; i < UI_SETTING_COUNT; ++i)
        if (reader.settings[i - UI_SETTING_FONT] != ui_settings_index(i)) return true;
    return false;
}
