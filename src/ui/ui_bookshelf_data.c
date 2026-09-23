#include "ui_bookshelf_data.h"
#include "lvgl.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#define STATE_DIR "/.epd_reader"
#define HISTORY_MAGIC 0x45505231u

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
    snprintf(out, size, STATE_DIR "/%08lx.%s", (unsigned long)hash(path, strlen(path)), suffix);
}

static bool load_slot(unsigned index, const char *slot, history_t *record)
{
    char path[64];
    ui_bookshelf_cache_path(index, path, sizeof(path), slot);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    int n = read(fd, record, sizeof(*record));
    close(fd);
    return n == sizeof(*record) && record->magic == HISTORY_MAGIC &&
           record->checksum == hash(record, offsetof(history_t, checksum)) &&
           record->size == books[index].file.size && record->modified == books[index].file.modified &&
           !memcmp(record->path, books[index].file.path, sizeof(record->path)) &&
           record->position.offset <= record->size && record->position.progress <= 100;
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
    history_t record;
    memset(&record, 0, sizeof(record));
    record.magic = HISTORY_MAGIC;
    record.sequence = ++order;
    books[index].order = order;
    record.size = books[index].file.size;
    record.modified = books[index].file.modified;
    memcpy(record.path, books[index].file.path, sizeof(record.path));
    record.position = *position;
    record.checksum = hash(&record, offsetof(history_t, checksum));
    mkdir(STATE_DIR, 0);
    char path[64];
    unsigned slot = saved_slots[index] ^ 1u;
    ui_bookshelf_cache_path(index, path, sizeof(path), slot ? "b" : "a");
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0);
    if (fd < 0) return false;
    bool ok = write(fd, &record, sizeof(record)) == sizeof(record);
    if (ok) ok = fsync(fd) == 0;
    if (close(fd) < 0) ok = false;
    if (ok) saved_slots[index] = slot;
    return ok;
}
