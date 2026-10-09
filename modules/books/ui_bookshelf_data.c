#include "ui_bookshelf_data.h"
#include "storage.h"
#include "storage_file.h"
#include "lvgl.h"
#include "document/document.h"
#include <rtdevice.h>
#include <rthw.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#define HISTORY_MAGIC 0x45505232u
#define HISTORY_LEGACY_MAGIC 0x45505231u

typedef struct
{
    uint32_t magic, sequence, size, modified;
    char path[BOOKSHELF_PATH_MAX];
    struct { uint32_t offset, current_page, total_pages, layout, progress, last_read; } position;
    uint32_t checksum;
} legacy_history_t;

typedef struct
{
    uint32_t magic, sequence, size, modified;
    char path[BOOKSHELF_PATH_MAX];
    ui_reading_position_t position;
    uint32_t checksum;
} history_t;

static ui_bookshelf_book_t *books;
static uint32_t order;
static unsigned char saved_slots[BOOKSHELF_MAX_ITEMS];
static unsigned count;
static bool storage_ready;

static struct
{
    struct rt_workqueue *queue;
    struct rt_work work;
    history_t pending;
    char path[64];
    unsigned index;
    bool dirty, suspended;
} writer;

/* Only the snapshot crosses threads; the worker never reads UI book objects. */
static bool write_pending(void)
{
    if (!storage_app_available("books")) return false;
    history_t record;
    char path[64];
    unsigned index, slot;
    rt_base_t level = rt_hw_interrupt_disable();
    if (!writer.dirty) { rt_hw_interrupt_enable(level); return true; }
    record = writer.pending;
    memcpy(path, writer.path, sizeof(path));
    index = writer.index;
    slot = saved_slots[index] ^ 1u;
    rt_hw_interrupt_enable(level);
    path[strlen(path) - 1] = slot ? 'b' : 'a';
    bool ok = storage_file_replace(path, &record, sizeof(record));
    level = rt_hw_interrupt_disable();
    if (ok)
    {
        saved_slots[index] = slot;
        if (writer.pending.sequence == record.sequence) writer.dirty = false;
    }
    rt_hw_interrupt_enable(level);
    return ok;
}

static void history_work(struct rt_work *work, void *parameter)
{
    (void)parameter;
    bool ok = write_pending();
    bool available = storage_app_available("books");
    rt_base_t level = rt_hw_interrupt_disable();
    if (writer.dirty && !writer.suspended && available)
        rt_workqueue_submit_work(writer.queue, work, rt_tick_from_millisecond(ok ? 100 : 1000));
    rt_hw_interrupt_enable(level);
}

bool ui_bookshelf_flush(void)
{
    if (!writer.queue) return !writer.dirty;
    rt_base_t level = rt_hw_interrupt_disable();
    writer.suspended = true;
    rt_hw_interrupt_enable(level);
    rt_workqueue_cancel_work_sync(writer.queue, &writer.work);
    /* A previously running callback may also have had a delayed retry queued. */
    rt_workqueue_cancel_work(writer.queue, &writer.work);
    bool ok = write_pending();
    bool available = storage_app_available("books");
    level = rt_hw_interrupt_disable();
    writer.suspended = false;
    if (writer.dirty && available)
        rt_workqueue_submit_work(writer.queue, &writer.work, rt_tick_from_millisecond(1000));
    rt_hw_interrupt_enable(level);
    return ok;
}

static uint32_t hash(const void *data, size_t length)
{
    const unsigned char *p = data;
    uint32_t h = 2166136261u;
    while (length--) h = (h ^ *p++) * 16777619u;
    return h;
}

void ui_bookshelf_cache_path(unsigned index, char *out, size_t size, const char *suffix)
{
    const char *path = books[index].file.path;
    const char *relative = storage_relative_path(path);
    char relative_name[32];
    snprintf(relative_name, sizeof(relative_name), "%s-%08lx.%s",
             !strcmp(storage_path_root(path), STORAGE_SD_ROOT) ? "sd" : "flash",
             (unsigned long)hash(relative, strlen(relative)), suffix);
    storage_app_path(out, size, "books", !strcmp(suffix, "idx") ? STORAGE_APP_CACHE : STORAGE_APP_DATA,
                     relative_name);
}

static bool load_slot(unsigned index, const char *slot, history_t *record)
{
    char path[64];
    ui_bookshelf_cache_path(index, path, sizeof(path), slot);
    storage_lock();
    int fd = storage_file_recover(path) ? open(path, O_RDONLY) : -1;
    bool legacy = false;
    if (fd < 0 && storage_app_available("books") && !strcmp(storage_path_root(books[index].file.path), STORAGE_SD_ROOT) &&
        storage_available(STORAGE_SD))
    {
        const char *relative = storage_relative_path(books[index].file.path);
        snprintf(path, sizeof(path), STORAGE_SD_ROOT "/.epd_reader/%08lx.%s",
                 (unsigned long)hash(relative, strlen(relative)), slot);
        fd = open(path, O_RDONLY);
        legacy = true;
    }
    if (fd < 0) { storage_unlock(); return false; }
    int n = read(fd, record, sizeof(*record));
    close(fd);
    bool integrity = n == sizeof(*record) && record->magic == HISTORY_MAGIC &&
                     record->checksum == hash(record, offsetof(history_t, checksum));
    if (n == sizeof(legacy_history_t) && record->magic == HISTORY_LEGACY_MAGIC && books[index].file.format == 0)
    {
        legacy_history_t old;
        memcpy(&old, record, sizeof(old));
        integrity = old.checksum == hash(&old, offsetof(legacy_history_t, checksum));
        memset(record, 0, sizeof(*record));
        record->magic = HISTORY_MAGIC; record->sequence = old.sequence;
        record->size = old.size; record->modified = old.modified;
        memcpy(record->path, old.path, sizeof(record->path));
        memcpy(&record->position, &old.position, sizeof(old.position));
        record->checksum = hash(record, offsetof(history_t, checksum));
        legacy = true;
    }
    bool ok = integrity &&
           record->size == books[index].file.size && record->modified == books[index].file.modified &&
           memchr(record->path, 0, sizeof(record->path)) &&
           !strcmp(storage_relative_path(record->path), storage_relative_path(books[index].file.path)) &&
           (books[index].file.format != 0 || record->position.offset <= record->size) &&
           record->position.progress <= 100 &&
           (books[index].file.format == 0 || (record->position.document_version == DOCUMENT_CONVERSION_VERSION &&
             record->position.document_kind == books[index].file.format));
    if (ok && legacy)
    {
        ui_bookshelf_cache_path(index, path, sizeof(path), slot);
        storage_file_replace(path, record, sizeof(*record));
    }
    storage_unlock();
    return ok;
}

static void format_last_read(ui_bookshelf_book_t *book)
{
    strcpy(book->last_read, book->position.current_page ? "已读" : "--");
    time_t stamp = book->position.last_read, now = time(NULL);
    struct tm then, today;
    if (stamp < 1704067200 || !localtime_r(&stamp, &then)) return;
    if (localtime_r(&now, &today) && then.tm_year == today.tm_year && then.tm_yday == today.tm_yday)
        strcpy(book->last_read, "今天");
    else strftime(book->last_read, sizeof(book->last_read), "%Y-%m-%d", &then);
}

int ui_bookshelf_refresh(void)
{
    if (!ui_bookshelf_flush()) return -RT_EIO;
    if (!writer.queue)
    {
        writer.queue = rt_workqueue_sysq();
        if (!writer.queue) return -RT_ENOMEM;
        rt_work_init(&writer.work, history_work, NULL);
    }
    if (!books) books = lv_malloc_zeroed(sizeof(*books) * BOOKSHELF_MAX_ITEMS);
    if (!books) return -RT_ENOMEM;
    ui_bookshelf_book_t *previous = lv_malloc(sizeof(*books) * BOOKSHELF_MAX_ITEMS);
    if (!previous) return -RT_ENOMEM;
    memcpy(previous, books, sizeof(*books) * BOOKSHELF_MAX_ITEMS);
    int scanned = bookshelf_refresh();
    storage_ready = scanned >= 0;
    count = scanned > 0 ? (unsigned)scanned : 0;
    for (unsigned i = 0; i < count; ++i)
    {
        bookshelf_item_t file;
        bookshelf_get(i, &file);
        ui_reading_position_t in_memory = {0};
        uint32_t previous_order = 0;
        /* Keep unsaved progress when a read-only filesystem is rescanned. */
        for (unsigned j = 0; j < BOOKSHELF_MAX_ITEMS; ++j)
            if (!strcmp(previous[j].file.path, file.path) && previous[j].file.size == file.size &&
                previous[j].file.modified == file.modified)
            {
                in_memory = previous[j].position;
                previous_order = previous[j].order;
                break;
            }
        memset(&books[i], 0, sizeof(books[i]));
        books[i].file = file;
        books[i].position = in_memory;
        history_t a, b;
        bool a_ok = load_slot(i, "a", &a), b_ok = load_slot(i, "b", &b);
        history_t *latest = a_ok ? &a : NULL;
        if (b_ok && (!latest || (int32_t)(b.sequence - latest->sequence) > 0)) latest = &b;
        saved_slots[i] = latest == &b ? 1 : 0;
        books[i].order = in_memory.current_page ? previous_order : latest ? latest->sequence : 0;
        if (latest && (!order || (int32_t)(latest->sequence - order) > 0)) order = latest->sequence;
        if (latest && !in_memory.current_page) books[i].position = latest->position;
        format_last_read(&books[i]);
    }
    lv_free(previous);
    return scanned;
}

unsigned ui_bookshelf_count(void) { return count; }
bool ui_bookshelf_storage_ready(void) { return storage_ready; }
const ui_bookshelf_book_t *ui_bookshelf_book(unsigned index) { return index < count ? &books[index] : NULL; }

bool ui_bookshelf_save(unsigned index, const ui_reading_position_t *position)
{
    if (index >= count) return false;
    books[index].position = *position;
    format_last_read(&books[index]);
    rt_base_t level = rt_hw_interrupt_disable();
    bool switching = writer.dirty && writer.index != index;
    rt_hw_interrupt_enable(level);
    if (switching && !ui_bookshelf_flush()) return false;
    history_t record;
    memset(&record, 0, sizeof(record));
    record.magic = HISTORY_MAGIC;
    record.sequence = ++order;
    books[index].order = order;
    if (!storage_app_available("books")) return false;
    record.size = books[index].file.size;
    record.modified = books[index].file.modified;
    memcpy(record.path, books[index].file.path, sizeof(record.path));
    record.position = *position;
    record.checksum = hash(&record, offsetof(history_t, checksum));
    char path[64];
    if (!writer.queue) return false;
    /* Switching books and rescanning flush before their indices can change. */
    ui_bookshelf_cache_path(index, path, sizeof(path), "a");
    level = rt_hw_interrupt_disable();
    writer.pending = record;
    memcpy(writer.path, path, sizeof(path));
    writer.index = index;
    writer.dirty = true;
    rt_workqueue_submit_work(writer.queue, &writer.work, rt_tick_from_millisecond(100));
    rt_hw_interrupt_enable(level);
    return true;
}

void ui_bookshelf_close(void)
{
    if (!ui_bookshelf_flush()) rt_kprintf("books: history flush failed\n");
    if (writer.queue)
    {
        rt_base_t level = rt_hw_interrupt_disable();
        writer.suspended = true;
        rt_hw_interrupt_enable(level);
        rt_workqueue_cancel_work_sync(writer.queue, &writer.work);
        rt_workqueue_cancel_work(writer.queue, &writer.work);
    }
    memset(&writer, 0, sizeof(writer));
    lv_free(books);
    books = NULL;
    count = order = 0;
    storage_ready = false;
}
