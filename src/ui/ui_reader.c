#include "ui_reader.h"
#include "ui_bookshelf_data.h"
#include "ui_settings.h"
#include "ui_font.h"
#include "ui_app.h"
#include "services/reader/reader.h"
#include "src/misc/lv_text_private.h"
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

#define TEXT_CAPACITY 16384u
#define WINDOW_CAPACITY 4096u
#define MAX_PAGES 65536u
#define INDEX_MAGIC 0x45504932u

typedef struct
{
    uint32_t magic, size, modified, layout, count, checksum;
    char path[BOOKSHELF_PATH_MAX];
} index_header_t;

static struct
{
    ui_reader_view_t view;
    reader_info_t file;
    lv_font_t *font, *fallback;
    char *text, *window, *scratch;
    uint32_t *mapping, *offsets;
    unsigned capacity, count, book, wanted;
    uint32_t layout, anchor;
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
    if (count > MAX_PAGES + 1) return false;
    if (count <= reader.capacity) return true;
    unsigned capacity = reader.capacity ? reader.capacity * 2 : 256;
    if (capacity < count) capacity = count;
    if (capacity > MAX_PAGES + 1) capacity = MAX_PAGES + 1;
    uint32_t *data = lv_realloc(reader.offsets, capacity * sizeof(*data));
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

/* The same LVGL line breaker and metrics are used for indexing and display.
 * The window retains source offsets even when GBK expands to UTF-8. */
static int page_measure(uint32_t start, char *output, uint32_t *end)
{
    unsigned used = 0, cursor = 0, copied = 0;
    uint32_t source = start;
    int step = reader.font->line_height + reader.view.line_space;
    unsigned lines = (UI_READER_BOTTOM - UI_READER_TOP + reader.view.line_space) / step;
    lv_text_attributes_t attr;
    lv_text_attributes_init(&attr);
    attr.max_width = 684 - 2 * reader.view.margin;
    attr.line_space = reader.view.line_space;
    reader.window[0] = '\0';
    reader.mapping[0] = start;
    for (unsigned line = 0; line < lines; ++line)
    {
        if (cursor)
        {
            memmove(reader.window, reader.window + cursor, used - cursor + 1);
            memmove(reader.mapping, reader.mapping + cursor, (used - cursor + 1) * sizeof(uint32_t));
            used -= cursor;
            cursor = 0;
        }
        while (used < WINDOW_CAPACITY - 8 && source < reader.file.file_size)
        {
            uint32_t next = source;
            int n = reader_read_mapped(source, reader.window + used, WINDOW_CAPACITY - used,
                                        reader.mapping + used, &next);
            if (n < 0 || next <= source) return -RT_EIO;
            source = next;
            used += n;
        }
        if (!used) break;
        unsigned n = lv_text_get_next_line(reader.window, used, reader.font, NULL, &attr);
        if (!n || n > used || copied + n + 1 >= TEXT_CAPACITY) return -RT_ERROR;
        if (output) memcpy(output + copied, reader.window, n);
        copied += n;
        /* Freeze soft line breaks so truncating the page cannot change
         * LVGL's look-ahead rules for an English word on the last line. */
        if (reader.window[n - 1] != '\n')
        {
            if (output) output[copied] = '\n';
            ++copied;
        }
        cursor = n;
        *end = reader.mapping[n];
    }
    if (!copied) *end = start;
    if (output) output[copied] = '\0';
    return (int)copied;
}

static bool index_load(void)
{
    char path[64];
    index_header_t header;
    const ui_bookshelf_book_t *book = ui_bookshelf_book(reader.book);
    ui_bookshelf_cache_path(reader.book, path, sizeof(path), "idx");
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    bool ok = read(fd, &header, sizeof(header)) == sizeof(header) &&
        header.magic == INDEX_MAGIC && header.size == reader.file.file_size &&
        header.modified == book->file.modified && header.layout == reader.layout &&
        !memcmp(header.path, book->file.path, sizeof(header.path)) &&
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
    if (ok) { reader.count = header.count; reader.complete = true; }
    return ok;
}

static void index_save(void)
{
    char path[64];
    index_header_t header;
    memset(&header, 0, sizeof(header));
    const ui_bookshelf_book_t *book = ui_bookshelf_book(reader.book);
    header.magic = INDEX_MAGIC;
    header.size = reader.file.file_size;
    header.modified = book->file.modified;
    header.layout = reader.layout;
    header.count = reader.count;
    header.checksum = offsets_hash(reader.offsets, reader.count + 1);
    memcpy(header.path, book->file.path, sizeof(header.path));
    mkdir("/.epd_reader", 0);
    ui_bookshelf_cache_path(reader.book, path, sizeof(path), "idx");
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0);
    if (fd < 0) return;
    unsigned bytes = (reader.count + 1) * sizeof(uint32_t);
    if (write(fd, &header, sizeof(header)) == sizeof(header) &&
        write(fd, reader.offsets, bytes) == bytes) fsync(fd);
    close(fd);
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
    time_t now = time(NULL);
    if (now >= 1704067200) position.last_read = (uint32_t)now;
    reader.view.saved = ui_bookshelf_save(reader.book, &position);
    reader_set_position(position.offset);
    ui_app_set_recent_reading(reader.view.title, position.progress);
}

static void show_page(void)
{
    uint32_t start = reader.offsets[reader.wanted], end = start;
    int n = page_measure(start, reader.scratch, &end);
    if (n < 0 || end != reader.offsets[reader.wanted + 1])
    {
        fail("读取失败，请检查存储后重新打开书籍");
        return;
    }
    memcpy(reader.text, reader.scratch, n + 1);
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
    static const unsigned sizes[] = {16, 18, 20, 22, 24, 28, 32, 36};
    static const unsigned margins[] = {4, 8, 12, 16, 20};
    uint32_t font_id;
    lv_font_t *fallback;
    lv_font_t *font = ui_font_reader_create(ui_settings_index(UI_SETTING_FONT),
                                          sizes[ui_settings_index(UI_SETTING_FONT_SIZE)],
                                          ui_settings_index(UI_SETTING_FONT_WEIGHT), &font_id, &fallback);
    if (!font) { fail("字体内存不足，无法打开书籍"); return false; }
    unsigned encoding = ui_settings_index(UI_SETTING_ENCODING);
    if (reader_set_encoding(encoding) != RT_EOK)
    {
        ui_font_reader_destroy(font);
        ui_font_reader_destroy(fallback);
        fail("文件编码设置失败");
        return false;
    }
    ui_font_reader_destroy(reader.font);
    ui_font_reader_destroy(reader.fallback);
    reader.font = font;
    reader.fallback = fallback;
    reader.view.font = font;
    reader.view.margin = 24 + margins[ui_settings_index(UI_SETTING_MARGIN)];
    reader.view.line_space = font->line_height * ui_settings_index(UI_SETTING_LINE_SPACING) / 5;
    /* Bump the seed when pagination or normalization rules change. */
    uint32_t layout[] = {5, font_id, reader.view.margin, reader.view.line_space, encoding,
                         UI_READER_BOTTOM - UI_READER_TOP, font->line_height};
    reader.layout = offsets_hash(layout, sizeof(layout) / sizeof(layout[0]));
    reader.anchor = anchor < reader.file.data_start ? reader.file.data_start : anchor;
    if (reader.anchor > reader.file.file_size) reader.anchor = reader.file.data_start;
    reader.restoring = true;
    reader.count = 0;
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
    if (reader.opened) position_save();
    reader_close();
    ui_font_reader_destroy(reader.font);
    ui_font_reader_destroy(reader.fallback);
    lv_free(reader.text); lv_free(reader.scratch); lv_free(reader.window);
    lv_free(reader.mapping); lv_free(reader.offsets);
    memset(&reader, 0, sizeof(reader));
}

bool ui_reader_open(unsigned book_index)
{
    ui_reader_close();
    const ui_bookshelf_book_t *book = ui_bookshelf_book(book_index);
    if (!book || reader_open(book->file.path) != RT_EOK) return false;
    reader.book = book_index;
    reader.opened = true;
    reader_get_info(&reader.file);
    reader.view.title = book->file.name;
    reader.text = lv_malloc(TEXT_CAPACITY);
    reader.scratch = lv_malloc(TEXT_CAPACITY);
    reader.window = lv_malloc(WINDOW_CAPACITY);
    reader.mapping = lv_malloc(WINDOW_CAPACITY * sizeof(uint32_t));
    if (!reader.text || !reader.scratch || !reader.window || !reader.mapping)
    { ui_reader_close(); return false; }
    reader.text[0] = '\0';
    reader.view.text = reader.text;
    if (!layout_prepare(book->position.offset)) { ui_reader_close(); return false; }
    ui_reader_process();
    return true;
}

bool ui_reader_reflow(void)
{
    if (!reader.opened) return false;
    uint32_t anchor = reader.view.ready ? reader.offsets[reader.view.page - 1] : reader.anchor;
    position_save();
    return layout_prepare(anchor);
}

void ui_reader_process(void)
{
    if (!reader.opened || reader.view.error[0]) return;
    reader_info_t current;
    if (!reader_get_info(&current) || strcmp(current.path, reader.file.path))
    {
        fail("书籍已被关闭或切换，请返回书架重开");
        return;
    }
    if (!reader.complete)
    {
        uint32_t start = reader.offsets[reader.count], end = start;
        int n = page_measure(start, NULL, &end);
        if (n < 0 || (end <= start && start < reader.file.file_size))
        { fail("读取失败，请检查存储后重新打开书籍"); return; }
        if (!reserve(reader.count + 2)) { fail("分页内存不足，无法继续排版"); return; }
        reader.offsets[++reader.count] = end;
        reader.complete = end >= reader.file.file_size;
        if (reader.complete) index_save();
    }
    reader.view.indexing = !reader.complete;
    reader.view.pages = reader.complete ? reader.count : 0;
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
    if (!reader.view.ready || reader.view.error[0] || !page) return;
    if (reader.complete && page > reader.count) page = reader.count;
    if (page > MAX_PAGES) page = MAX_PAGES;
    reader.wanted = page - 1;
}

void ui_reader_turn(int direction)
{
    if (!reader.view.ready) return;
    int next = (int)reader.view.page + direction;
    if (next < 1) next = 1;
    ui_reader_seek((unsigned)next);
}

const ui_reader_view_t *ui_reader_view(void) { return &reader.view; }
void ui_reader_save(void) { position_save(); }
bool ui_reader_waiting(void)
{
    return !reader.view.error[0] && (!reader.view.ready || reader.restoring ||
                                    reader.wanted + 1 != reader.view.page);
}
