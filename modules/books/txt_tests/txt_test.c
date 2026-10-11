#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdint.h>
#include <stdbool.h>

static unsigned reads, seeks, writes, syncs, line_calls, display_lines, index_lines;
static size_t read_bytes, short_read;
static int tracked_fd = -1;
static unsigned tracked_reads;
static bool media_ok = true, fail_read, remove_during_read;
static bool sd_volume;
static uint32_t card_session = 1;
static ssize_t traced_read(int fd, void *data, size_t n)
{
    ++reads;
    if (fd == tracked_fd) ++tracked_reads;
    if (fail_read) return -1;
    if (short_read && n > short_read) n = short_read;
    ssize_t result = read(fd, data, n);
    if (result > 0) read_bytes += result;
    if (remove_during_read) media_ok = false;
    return result;
}
static off_t traced_seek(int fd, off_t off, int whence)
{ ++seeks; return lseek(fd, off, whence); }
static ssize_t traced_write(int fd, const void *data, size_t n)
{ ++writes; return write(fd, data, n); }
static int traced_sync(int fd) { ++syncs; return fsync(fd); }
#define read traced_read
#define lseek traced_seek
#define write traced_write
#define fsync traced_sync
#include "../reader.c"
#include "../ui_reader.c"
#undef read
#undef lseek
#undef write
#undef fsync

static size_t fail_size;
static unsigned fail_match, size_matches, alloc_count, fail_alloc;
static unsigned saves, async_calls, prepared_calls, waits;
static bool fail_snapshot, fail_glyphs, non_txt;
static unsigned delegation_budget;
static bool delegation_foreground, delegation_commit;
static char directory[] = "/tmp/txt-fixture.XXXXXX", cache_path[64];
static ui_bookshelf_book_t book;
static lv_font_t test_font = { .line_height = 24 };
static unsigned settings[UI_SETTING_COUNT];
static unsigned char *snapshot;
static size_t snapshot_size;
static unsigned char prepared_seen[CHARACTER_SET_BYTES];

void *epd_app_alloc(size_t n, int region)
{
    (void)region;
    if (++alloc_count == fail_alloc) return NULL;
    if (n == fail_size && (++size_matches == fail_match || !fail_match)) return NULL;
    return malloc(n);
}
void *epd_app_realloc(void *p, size_t n, int region)
{
    (void)region;
    if (++alloc_count == fail_alloc) return NULL;
    return realloc(p, n);
}
void epd_app_free(void *p) { free(p); }
rt_mutex_t rt_mutex_create(const char *name, int policy) { (void)name; (void)policy; return (void *)1; }
int rt_mutex_take(rt_mutex_t lock, int wait) { assert(lock); (void)wait; return 0; }
int rt_mutex_release(rt_mutex_t lock) { assert(lock); return 0; }
int rt_mutex_delete(rt_mutex_t lock) { assert(lock); return 0; }
rt_tick_t rt_tick_get(void) { static uint32_t tick; return ++tick; }
rt_tick_t rt_tick_from_millisecond(int ms) { return ms; }
void storage_lock(void) {}
void storage_unlock(void) {}
bool storage_path_available(const char *path) { (void)path; return media_ok; }
const char *storage_path_root(const char *path) { (void)path; return sd_volume ? STORAGE_SD_ROOT : STORAGE_FLASH_ROOT; }
uint32_t storage_card_session(void) { return card_session; }
bool storage_session_valid(storage_volume_t volume, uint32_t session)
{ return media_ok && (volume != STORAGE_SD || session == card_session); }
bool storage_app_available(const char *id) { (void)id; return media_ok; }
bool storage_file_recover(const char *path) { (void)path; return true; }
const char *storage_relative_path(const char *path) { return path; }
const ui_bookshelf_book_t *ui_bookshelf_book(unsigned i) { assert(!i); return &book; }
void ui_bookshelf_cache_path(unsigned i, char *out, size_t n, const char *ext)
{ assert(!i); (void)ext; snprintf(out, n, "%s", cache_path); }
bool ui_bookshelf_save(unsigned i, const ui_reading_position_t *position)
{ assert(!i); ++saves; book.position = *position; return true; }
bool ui_bookshelf_flush(void) { return true; }
unsigned ui_settings_index(ui_setting_id_t id) { return settings[id]; }
lv_font_t *epd_app_font_create(unsigned id, unsigned size, unsigned weight, uint32_t *signature, lv_font_t **fallback)
{ (void)id; (void)weight; *signature = 123 + size; *fallback = NULL; return &test_font; }
void epd_app_font_destroy(lv_font_t *font) { (void)font; }
void epd_app_set_recent_reading(const char *title, int progress) { (void)title; (void)progress; }
bool lv_font_get_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *d, uint32_t c, uint32_t next)
{
    memset(d, 0, sizeof(*d));
    if (font->get_glyph_dsc) return font->get_glyph_dsc(font, d, c, next);
    d->adv_w = c >= 128 ? 24 : 12;
    return true;
}
void lv_draw_wait_for_finish(void) { ++waits; }
bool epd_app_font_prepare(const lv_font_t *font, uint32_t c, bool adjacent)
{
    assert(font && c >= 0x20 && waits);
    (void)adjacent;
    ++prepared_calls;
    prepared_seen[c >> 3] |= 1u << (c & 7);
    return !fail_glyphs;
}
void lv_text_attributes_init(lv_text_attributes_t *attr) { memset(attr, 0, sizeof(*attr)); }
unsigned lv_text_get_next_line(const char *text, unsigned bytes, const lv_font_t *font,
                              void *unused, const lv_text_attributes_t *attr)
{
    (void)unused;
    ++line_calls;
    uintptr_t address = (uintptr_t)text;
    if (address >= (uintptr_t)reader.display_stream.text &&
        address < (uintptr_t)reader.display_stream.text + STREAM_CAPACITY) ++display_lines;
    if (address >= (uintptr_t)reader.index_stream.text &&
        address < (uintptr_t)reader.index_stream.text + STREAM_CAPACITY) ++index_lines;
    unsigned pos = 0, space = 0, width = 0;
    while (pos < bytes)
    {
        unsigned start = pos;
        uint32_t c = next_letter(text, &pos);
        if (c == '\n') return pos;
        lv_font_glyph_dsc_t glyph;
        lv_font_get_glyph_dsc(font, &glyph, c, 0);
        if (width + glyph.adv_w > (unsigned)attr->max_width && start) return space ? space : start;
        width += glyph.adv_w;
        if (c == ' ') space = pos;
    }
    return pos;
}
unsigned short ff_oem2uni(unsigned short c, unsigned short cp)
{ assert(cp == 936); return c == 0xd6d0 ? 0x4e2d : c == 0xcec4 ? 0x6587 : 0; }
bool document_save_async(const char *path, const void *header, size_t hn, const void *data, size_t n)
{
    assert(!strcmp(path, cache_path)); ++async_calls;
    if (fail_snapshot) return false;
    free(snapshot); snapshot_size = hn + n; snapshot = malloc(snapshot_size); assert(snapshot);
    memcpy(snapshot, header, hn); memcpy(snapshot + hn, data, n);
    return true;
}
bool document_view_active(void) { return non_txt; }
ui_reader_work_t document_view_step(uint32_t ms, bool fg, bool commit)
{ delegation_budget = ms; delegation_foreground = fg; delegation_commit = commit; return UI_READER_WORK_TARGET; }
bool document_view_open(unsigned i) { (void)i; return false; }
void document_view_close(void) {}
bool document_view_save(void) { return true; }
bool document_view_reflow(void) { return true; }
bool document_view_settings_changed(void) { return false; }
void document_view_seek(unsigned page) { (void)page; }
void document_view_cancel_pending(void) {}
bool document_view_waiting(void) { return false; }
const ui_reader_view_t *document_view_get(void) { return &reader.view; }

static void snapshot_write(void)
{
    assert(snapshot);
    int fd = open(cache_path, O_CREAT | O_TRUNC | O_WRONLY, 0600); assert(fd >= 0);
    assert(write(fd, snapshot, snapshot_size) == (ssize_t)snapshot_size); close(fd);
}
static void fixture(const void *bytes, size_t n)
{
    ui_reader_close(); unlink(cache_path);
    memset(&book, 0, sizeof(book));
    snprintf(book.file.path, sizeof(book.file.path), "%s/book.txt", directory);
    strcpy(book.file.name, "TXT host fixture"); book.file.format = DOCUMENT_TXT;
    book.file.modified = 7; book.file.size = n;
    int fd = open(book.file.path, O_CREAT | O_TRUNC | O_WRONLY, 0600); assert(fd >= 0);
    assert(write(fd, bytes, n) == (ssize_t)n); close(fd);
    reads = seeks = writes = syncs = line_calls = display_lines = index_lines = 0;
    read_bytes = short_read = fail_size = 0;
    tracked_fd = -1; tracked_reads = 0;
    fail_match = size_matches = alloc_count = fail_alloc = 0;
    saves = async_calls = prepared_calls = waits = 0;
    fail_snapshot = fail_glyphs = fail_read = remove_during_read = false; media_ok = true;
    memset(settings, 0, sizeof(settings)); memset(prepared_seen, 0, sizeof(prepared_seen));
    free(snapshot); snapshot = NULL; snapshot_size = 0;
}
static void drain(bool foreground, bool commit)
{
    unsigned iterations = 0;
    while (ui_reader_step(8, foreground, commit)) assert(++iterations < 100000);
    assert(!reader.error[0]);
}
static void make_book(void)
{
    const char *line = "English words mixed with \xe4\xb8\xad\xe6\x96\x87 and \xf0\x9f\x98\x80.\r\n\n\twordlonglonglonglong\r";
    size_t len = strlen(line), n = 1600 * len + 3;
    char *data = malloc(n); assert(data); memcpy(data, "\xef\xbb\xbf", 3);
    for (unsigned i = 0; i < 1600; ++i) memcpy(data + 3 + i * len, line, len);
    fixture(data, n); free(data);
}

static void cursors_test(void)
{
    make_book(); assert(reader_open(book.file.path) == RT_EOK);
    reader_cursor_t *a = reader_cursor_create(), *b = reader_cursor_create(); assert(a && b);
    char text[4096], other[4096]; uint32_t map[4096], next, position = 3;
    short_read = 137; reads = seeks = 0; read_bytes = 0;
    size_t sequential_bytes = 0; unsigned sequential_seeks = 0;
    while (position < book.file.size)
    {
        size_t before = read_bytes; unsigned before_seek = seeks;
        int n = reader_cursor_read(a, position, text, sizeof(text), map, &next); assert(n > 0 && next > position);
        sequential_bytes += read_bytes - before; sequential_seeks += seeks - before_seek;
        assert(map[n] == next && map[0] == position);
        uint32_t end;
        assert(reader_cursor_read(b, position, other, sizeof(other), NULL, &end) == n);
        assert(end == next && !strcmp(text, other));
        assert(reader_read_text(3, other, sizeof(other), &end) > 0);
        position = next;
    }
    assert(sequential_bytes == book.file.size - 3 && sequential_seeks == 1);
    reader_close(); assert(reader_open(book.file.path) == RT_EOK);
    assert(reader_cursor_read(a, 3, text, sizeof(text), NULL, &next) < 0);
    reader_cursor_destroy(a); reader_cursor_destroy(b);
    fail_size = RAW_PREFETCH_SIZE; assert(!reader_cursor_create()); fail_size = 0;
    assert(reader_cursor_read(NULL, 3, text, sizeof(text), NULL, &next) > 0);
    fail_read = true; assert(reader_read_text(80000, text, sizeof(text), &next) < 0); fail_read = false;
    remove_during_read = true; assert(reader_read_text(80000, text, sizeof(text), &next) < 0);
    remove_during_read = false; media_ok = true;
    reader_close(); sd_volume = true; assert(reader_open(book.file.path) == RT_EOK);
    assert(reader_read_text(3, text, sizeof(text), &next) > 0); ++card_session;
    assert(reader_read_text(3, text, sizeof(text), &next) < 0);
    reader_info_t info; assert(!reader_get_info(&info)); sd_volume = false;
    printf("cursors: sequential bytes=%zu, seeks=%u; fallback/short read/removal passed\n", sequential_bytes, sequential_seeks);
}

static void decoding_test(void)
{
    const unsigned char input[] = {0xef,0xbb,0xbf,'A','\r','\n','\t',0xe4,0xb8,0xad,0xf0,0x9f,0x98,0x80,0xff,'\r','B'};
    fixture(input, sizeof(input)); assert(reader_open(book.file.path) == RT_EOK);
    char out[128]; uint32_t map[128], next;
    const uint32_t expected[] = {3,4,6,7,7,7,10,10,10,10,14,15,16,17};
    int n = reader_read_mapped(0, out, sizeof(out), map, &next);
    assert(n == 13 && !strcmp(out, "A\n \xe4\xb8\xad\xf0\x9f\x98\x80?\nB"));
    assert(!memcmp(map, expected, sizeof(expected)) && next == sizeof(input));
    const unsigned char gbk[] = {'A',0xd6,0xd0,0xce,0xc4,'\r','\n',0x81,0x40};
    fixture(gbk, sizeof(gbk)); assert(reader_open(book.file.path) == RT_EOK);
    n = reader_read_mapped(0, out, sizeof(out), map, &next);
    assert(n == 9 && !strcmp(out, "A\xe4\xb8\xad\xe6\x96\x87\n?") && next == sizeof(gbk));
    assert(map[1] == 1 && map[4] == 3 && map[7] == 5 && map[8] == 7 && map[9] == 9);
    for (unsigned boundary = 1023; boundary <= 8191; boundary += 7168)
    {
        char *data = malloc(boundary + 16); memset(data, 'a', boundary);
        memcpy(data + boundary, "\r\n\xf0\x9f\x98\x80\xe4\xb8\xad", 9);
        fixture(data, boundary + 9); free(data); assert(reader_open(book.file.path) == RT_EOK);
        uint32_t at = 0; unsigned total = 0;
        while (at < book.file.size)
        { n = reader_read_mapped(at, out, sizeof(out), map, &next); assert(n > 0 && next > at); total += n; at = next; }
        assert(total == boundary + 8);
    }
    puts("decoding: BOM/CRLF/UTF-8/GBK/source offsets and block crossings passed");
}

static void scheduling_test(void)
{
    make_book(); book.position.offset = 40000;
    ui_reading_position_t old = book.position;
    assert(ui_reader_open(0)); unsigned revision = reader.view.revision;
    tracked_fd = reader.ahead_cursor->fd;
    while (!target_ready())
    {
        unsigned before = line_calls + prepared_calls;
        assert(ui_reader_step(1, true, false));
        assert(line_calls + prepared_calls <= before + 1);
    }
    assert(display_lines == 0 && reader.pending.valid && !reader.view.ready && !saves);
    assert(reader.ahead_source > 3 && reader.pending_characters && prepared_calls == 0);
    drain(true, false);
    assert(reader.complete && !reader.view.ready && reader.view.revision == revision);
    assert(!memcmp(&old, &book.position, sizeof(old)) && reader_get_position() == 3);
    assert(!ui_reader_step(8, true, false) && async_calls == 1 && !writes && !syncs);
    assert(reader.ahead_source == book.file.size && !reader.pending_characters);
    assert(!tracked_reads);
    unsigned calls = line_calls;
    assert(!ui_reader_step(0, true, true));
    assert(reader.view.ready && saves == 1 && reader.view.page == reader.wanted + 1);
    assert(line_calls == calls && display_lines == 0 && reader.view.revision == revision + 1);
    assert(book.position.offset == reader.offsets[reader.wanted]);
    ui_reading_position_t actual = book.position; revision = reader.view.revision;
    ui_reader_seek(5); ui_reader_step(1, true, false); ui_reader_seek(8); drain(true, false);
    assert(!memcmp(&actual, &book.position, sizeof(actual)) && reader.view.revision == revision);
    ui_reader_step(0, true, true); assert(reader.view.page == 8 && book.position.offset == reader.offsets[7]);
    ui_reader_seek(12); drain(true, false); ui_reader_cancel_pending(); drain(true, true);
    assert(reader.view.page == 8);
    unsigned total = reader.count;
    index_header_t header; memcpy(&header, snapshot, sizeof(header));
    assert(header.magic == INDEX_MAGIC && header.count == total && header.layout == reader.layout);
    assert(snapshot_size == sizeof(header) + (total + 1) * sizeof(uint32_t));
    assert(!memcmp(snapshot + sizeof(header), reader.offsets, (total + 1) * sizeof(uint32_t)));
    ui_reader_close(); snapshot_write();
    saves = 0; assert(ui_reader_open(0)); assert(reader.complete && !reader.discover_index);
    drain(false, true); assert(!reader.view.ready && !saves && reader.view.pages == total);
    assert(prepared_calls && reader.ahead_source == book.file.size);
    ui_reader_step(0, true, true); assert(reader.view.page == 8);
    puts("scheduler: busy commit isolation, final target, restore, cancel, async byte compatibility and cache hit passed");
}

static void footer_and_failure_test(void)
{
    make_book(); assert(ui_reader_open(0));
    while (!reader.view.ready) ui_reader_step(1, true, true);
    assert(!reader.complete); unsigned revision = reader.view.revision, page = reader.view.page;
    unsigned before_save = saves;
    drain(false, true);
    assert(reader.view.pages == reader.count && !reader.view.indexing && reader.view.page == page);
    assert(reader.view.revision == revision && saves == before_save);
    ui_reader_step(0, true, true);
    assert(book.position.total_pages == reader.count && saves == before_save + 1);
    make_book(); fail_snapshot = true; fail_glyphs = true;
    assert(ui_reader_open(0)); drain(true, true);
    assert(reader.complete && reader.view.ready && !snapshot && async_calls == 1 && !reader.pending_characters);
    unsigned prepared = prepared_calls; assert(!ui_reader_step(8, true, true) && prepared == prepared_calls);
    media_ok = false; revision = reader.view.revision; ui_reader_step(8, true, false);
    assert(reader.error[0] && !reader.view.error[0] && reader.view.revision == revision);
    ui_reader_step(0, false, true); assert(reader.view.error[0] && reader.view.revision == revision + 1);
    media_ok = true;
    puts("footer/failures: background metadata, unchanged body revision, optional snapshot/glyph failure and deferred errors passed");
}

static void target_priority_test(void)
{
    make_book(); assert(ui_reader_open(0));
    assert(ui_reader_step(0, true, false) == UI_READER_WORK_TARGET);
    while (!target_ready()) ui_reader_step(1, true, false);
    unsigned lines = line_calls, prepared = prepared_calls;
    assert(ui_reader_step(8, true, true) == UI_READER_WORK_BACKGROUND);
    assert(reader.view.ready && line_calls == lines && prepared_calls == prepared);
    assert(!reader.complete); /* A ready target does not spend the remaining budget on prefetch. */
    ui_reader_seek(8);
    assert(ui_reader_step(0, true, false) == UI_READER_WORK_TARGET);
    ui_reader_cancel_pending();
    assert(ui_reader_step(0, true, true) == UI_READER_WORK_BACKGROUND);
    drain(true, true);
    assert(ui_reader_step(8, true, true) == UI_READER_WORK_IDLE);
    puts("priority: required target, ready-page immediate commit, optional work and idle passed");
}

static void allocations_test(void)
{
    for (unsigned set = 1; set <= 2; ++set)
    {
        make_book(); fail_size = CHARACTER_SET_BYTES; fail_match = set;
        assert(ui_reader_open(0)); assert(!reader.discover_index); drain(true, true);
        assert(reader.view.ready && reader.ahead_source == book.file.size);
    }
    make_book(); fail_size = CHARACTER_SET_BYTES;
    assert(ui_reader_open(0)); assert(!reader.characters && !reader.discovered); drain(true, true);
    make_book(); fail_size = RAW_PREFETCH_SIZE;
    /* The decoded streams have the same size; fail only raw cursor windows. */
    fail_match = 4; assert(ui_reader_open(0)); assert(!reader.index_stream.cursor); drain(true, true);
    make_book(); fail_size = TEXT_CAPACITY; fail_match = 3;
    assert(ui_reader_open(0)); assert(!reader.index_text); drain(true, true); assert(display_lines > 0);
    unsigned allocations = alloc_count;
    for (unsigned i = 1; i <= allocations; ++i)
    {
        fixture("short book\n", 11); fail_alloc = i;
        if (ui_reader_open(0)) drain(true, true);
        ui_reader_close(); assert(!g_cursors);
    }
    non_txt = true; assert(ui_reader_step(3, false, true));
    assert(delegation_budget == 3 && !delegation_foreground && delegation_commit); non_txt = false;
    puts("allocations: both character sets, optional cursor/collector, per-allocation failure cleanup and document delegation passed");
}

/* This reference lays out the entire decoded fixture independently of the
 * incremental streams and caches, using the same deterministic font metric. */
static void reference_pages(void)
{
    size_t capacity = book.file.size * 4 + 8;
    char *text = malloc(capacity), page_text[TEXT_CAPACITY];
    uint32_t *map = malloc(capacity * sizeof(*map)); assert(text && map);
    uint32_t offset = reader.file.data_start; unsigned bytes = 0;
    map[0] = offset;
    while (offset < book.file.size)
    {
        uint32_t next = offset;
        int n = reader_read_mapped(offset, text + bytes, capacity - bytes, map + bytes, &next);
        assert(n > 0 && next > offset); offset = next; bytes += n;
    }
    text[bytes] = 0;
    lv_text_attributes_t attr = {684 - 2 * reader.view.margin, reader.view.line_space};
    unsigned head = 0, page = 0;
    do
    {
        assert(reader.offsets[page] == map[head]); unsigned copied = 0;
        for (unsigned line = 0; line < page_lines() && head < bytes; ++line)
        {
            unsigned n = lv_text_get_next_line(text + head, bytes - head, &test_font, NULL, &attr);
            assert(n && copied + n + 1 < TEXT_CAPACITY);
            memcpy(page_text + copied, text + head, n); copied += n;
            if (text[head + n - 1] != '\n') page_text[copied++] = '\n';
            head += n;
        }
        page_text[copied] = 0;
        assert(reader.offsets[page + 1] == map[head]);
        ui_reader_seek(page + 1);
        unsigned turns = 0;
        do { ui_reader_step(8, true, true); assert(++turns < 100000); }
        while (reader.view.page != page + 1);
        assert(!strcmp(reader.view.text, page_text)); ++page;
    } while (head < bytes);
    assert(page == reader.count);
    free(text); free(map);
}

static void pagination_test(void)
{
    make_book(); assert(ui_reader_open(0)); drain(true, true);
    assert(prepared_find(0) && prepared_find(1));
    ui_reader_seek(2); ui_reader_step(1, true, false);
    assert(prepared_find(0)); drain(true, true);
    assert(prepared_find(0) && prepared_find(1) && prepared_find(2));
    reference_pages();
    uint32_t old_offset = book.position.offset;
    settings[UI_SETTING_MARGIN] = 4; settings[UI_SETTING_LINE_SPACING] = 2;
    assert(ui_reader_settings_changed() && ui_reader_reflow());
    ui_reading_position_t position = book.position;
    drain(true, false); assert(!memcmp(&position, &book.position, sizeof(position)));
    ui_reader_step(0, true, true);
    assert(reader.offsets[reader.wanted] <= old_offset &&
        (reader.offsets[reader.wanted + 1] > old_offset || old_offset == book.file.size));
    reference_pages();
    const char gbk[] = "ASCII words \xd6\xd0\xce\xc4\r\n\r\n";
    size_t len = sizeof(gbk) - 1; char *data = malloc(len * 400);
    for (unsigned i = 0; i < 400; ++i) memcpy(data + len * i, gbk, len);
    fixture(data, len * 400); free(data); assert(ui_reader_open(0)); drain(true, true); reference_pages();
    fixture("", 0); assert(ui_reader_open(0)); drain(true, true); reference_pages();
    assert(reader.count == 1 && reader.view.percent == 0);
    fixture("\xef\xbb\xbf", 3); assert(ui_reader_open(0)); drain(true, true); reference_pages();
    /* Actual cache allocation loss must not make the two neighbours repeatedly
     * evict each other and keep the scheduler runnable forever. */
    for (unsigned slots = 0; slots < PAGE_CACHE_COUNT; ++slots)
    {
        make_book(); assert(ui_reader_open(0));
        for (unsigned i = slots; i < PAGE_CACHE_COUNT; ++i)
        { epd_app_free(reader.prepared[i].text); memset(&reader.prepared[i], 0, sizeof(reader.prepared[i])); }
        drain(true, true); ui_reader_seek(8); drain(true, true);
        ui_reader_seek(7); drain(true, true); assert(reader.view.page == 7);
    }
    puts("pagination: all source boundaries/text, UTF-8/GBK, reflow anchor, empty/BOM and 0/1/2 optional slots passed");
}

int main(void)
{
    assert(mkdtemp(directory)); snprintf(cache_path, sizeof(cache_path), "%s/book.idx", directory);
    assert(reader_service_init() == RT_EOK);
    cursors_test(); decoding_test(); scheduling_test(); target_priority_test(); footer_and_failure_test(); allocations_test(); pagination_test();
    ui_reader_close(); reader_service_deinit(); free(snapshot);
    unlink(book.file.path); unlink(cache_path); assert(!rmdir(directory));
    /* The production UI path cannot synchronously write or sync index files. */
    assert(!writes && !syncs); (void)traced_write; (void)traced_sync;
    puts("TXT host tests passed");
    return 0;
}
