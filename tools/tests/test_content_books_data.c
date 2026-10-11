#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <io.h>
#include "constants.h"
typedef int rt_err_t;
typedef intptr_t rt_base_t;
typedef bool rt_bool_t;
#include "books_data_types.h"

enum { STORAGE_FLASH, STORAGE_SD, STORAGE_APP_CACHE, STORAGE_APP_DATA };
#define DOCUMENT_CONVERSION_VERSION 2
#define DOCUMENT_UNKNOWN UINT32_MAX
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#define lv_malloc malloc
#define lv_malloc_zeroed(size) calloc(1, size)
#define lv_free free
#define rt_kprintf printf
#define open _open
#define read _read
#define close _close
#undef O_RDONLY
#define O_RDONLY (_O_RDONLY | _O_BINARY)
struct rt_work { int dummy; };
struct rt_workqueue { int dummy; };
static struct rt_workqueue queue;
static bookshelf_item_t fixture[BOOKSHELF_MAX_ITEMS];
static unsigned fixture_count, writes, pending_jobs;
static int lock_depth;
static bool medium_available = true, changing, writable = true;
static uint32_t modified = 17;
static void storage_lock(void) { ++lock_depth; }
static void storage_unlock(void) { assert(lock_depth > 0); --lock_depth; }
static bool storage_changing(void) { return changing; }
static bool storage_path_available(const char *path) { (void)path; return medium_available; }
static bool storage_app_available(const char *id) { (void)id; return writable; }
static bool storage_available(int volume) { (void)volume; return medium_available; }
static const char *storage_path_root(const char *path)
{ return !strncmp(path, "/sdcard/", 8) ? STORAGE_SD_ROOT : STORAGE_FLASH_ROOT; }
static const char *storage_relative_path(const char *path)
{ return path + strlen(storage_path_root(path)); }
static bool storage_file_signature(const char *path, uint32_t *value)
{ (void)path; *value = modified; return medium_available; }
static bool storage_file_recover(const char *path) { (void)path; return true; }
static bool storage_app_path(char *out, size_t size, const char *id, int area, const char *relative)
{
    (void)id; (void)area;
    int n = snprintf(out, size, "%s", relative);
    return n > 0 && (size_t)n < size;
}
static bool storage_file_replace(const char *path, const void *record, size_t size)
{
    if (!writable) return false;
    FILE *file = fopen(path, "wb");
    assert(file);
    bool ok = fwrite(record, 1, size, file) == size;
    fclose(file);
    ++writes;
    return ok;
}
static int host_stat(const char *path, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_mode = _S_IFREG;
    st->st_size = 100;
    st->st_mtime = modified;
    return medium_available && strstr(path, "/missing/") == NULL ? 0 : -1;
}
#define stat(path, st) host_stat(path, st)
static uint32_t document_format(const char *path, bool directory)
{
    (void)directory;
    const char *ext = strrchr(path, '.');
    return ext && !strcmp(ext, ".txt") ? 0 : ext && !strcmp(ext, ".epub") ? 2 : DOCUMENT_UNKNOWN;
}
static rt_base_t rt_hw_interrupt_disable(void) { return 0; }
static void rt_hw_interrupt_enable(rt_base_t level) { (void)level; }
static struct rt_workqueue *rt_workqueue_sysq(void) { return &queue; }
static void rt_work_init(struct rt_work *work, void (*cb)(struct rt_work *, void *), void *arg)
{ (void)work; (void)cb; (void)arg; }
static void rt_workqueue_cancel_work_sync(struct rt_workqueue *q, struct rt_work *w)
{ (void)q; (void)w; pending_jobs = 0; }
static void rt_workqueue_cancel_work(struct rt_workqueue *q, struct rt_work *w)
{ (void)q; (void)w; pending_jobs = 0; }
static unsigned rt_tick_from_millisecond(unsigned ms) { return ms; }
static void rt_workqueue_submit_work(struct rt_workqueue *q, struct rt_work *w, unsigned ticks)
{ (void)q; (void)w; (void)ticks; ++pending_jobs; }
static struct tm *localtime_r(const time_t *stamp, struct tm *out)
{ return localtime_s(out, stamp) ? NULL : out; }
int bookshelf_refresh(void) { return (int)fixture_count; }
bool bookshelf_get(int index, bookshelf_item_t *out)
{
    if (index < 0 || (unsigned)index >= fixture_count) return false;
    *out = fixture[index];
    return true;
}
#include "books_data_under_test.h"

int main(void)
{
    fixture_count = BOOKSHELF_MAX_ITEMS;
    for (unsigned i = 0; i < fixture_count; ++i)
    {
        snprintf(fixture[i].path, sizeof(fixture[i].path), "/sdcard/root%u.txt", i);
        snprintf(fixture[i].name, sizeof(fixture[i].name), "Root %u", i);
        fixture[i].size = 100;
        fixture[i].modified = modified;
    }
    assert(ui_bookshelf_refresh() == BOOKSHELF_MAX_ITEMS);
    unsigned index;
    assert(ui_bookshelf_include_path("/sdcard/root5.txt", &index) && index == 5);
    const char *path = "/sdcard/中文 目录/外部 书籍.txt";
    assert(ui_bookshelf_include_path(path, &index) && index == BOOKSHELF_MAX_ITEMS);
    assert(ui_bookshelf_count() == BOOKSHELF_MAX_ITEMS);
    assert(!strcmp(ui_bookshelf_book(index)->file.path, path));
    assert(!strcmp(ui_bookshelf_book(index)->file.name, "外部 书籍"));
    char cache[64];
    ui_bookshelf_cache_path(index, cache, sizeof(cache), "idx");
    assert(!strncmp(cache, "sd-", 3));
    ui_reading_position_t saved = {.offset = 45, .current_page = 4, .total_pages = 10, .progress = 40};
    assert(ui_bookshelf_save(index, &saved));
    assert(pending_jobs && ui_bookshelf_flush() && writes == 1);
    assert(ui_bookshelf_include_path("/sdcard/other/book.txt", &index));
    assert(ui_bookshelf_book(index)->position.current_page == 0);
    assert(ui_bookshelf_include_path(path, &index));
    assert(ui_bookshelf_book(index)->position.current_page == 4);
    assert(ui_bookshelf_book(index)->position.offset == 45);
    assert(ui_bookshelf_save(index, &saved));
    writable = false;
    assert(!ui_bookshelf_include_path("/sdcard/other/book.txt", &index));
    assert(!strcmp(ui_bookshelf_book(index)->file.path, path));
    writable = true;
    assert(ui_bookshelf_flush());
    medium_available = false;
    assert(!ui_bookshelf_include_path(path, &index));
    medium_available = true;
    changing = true;
    assert(!ui_bookshelf_include_path(path, &index));
    changing = false;
    assert(!ui_bookshelf_include_path("/sdcard/missing/book.txt", &index));
    assert(!ui_bookshelf_include_path("/sdcard/a.png", &index));
    ++modified;
    assert(ui_bookshelf_include_path(path, &index));
    assert(ui_bookshelf_book(index)->position.current_page == 0);
    assert(ui_bookshelf_include_path("/flash/中文 目录/一本.epub", &index));
    assert(ui_bookshelf_book(index)->file.format == 2);
    saved.document_version = DOCUMENT_CONVERSION_VERSION;
    saved.document_kind = 2;
    assert(ui_bookshelf_save(index, &saved) && ui_bookshelf_flush());
    ui_bookshelf_close();
    assert(!ui_bookshelf_book(BOOKSHELF_MAX_ITEMS));
    assert(!pending_jobs && !lock_depth);
    assert(ui_bookshelf_include_path("/flash/中文 目录/一本.epub", &index));
    assert(ui_bookshelf_book(index)->position.current_page == 4);
    ui_bookshelf_close();
    puts("books data: full shelf, nested UTF-8 paths, history restore/flush, replaced identity and cleanup passed");
    return 0;
}
