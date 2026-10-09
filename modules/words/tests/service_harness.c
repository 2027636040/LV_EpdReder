/* Host-only in-memory filesystem. The production worker/queue/store code is included unchanged. */
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include "platform/epd_app.h"
#include "dfs_posix.h"
static time_t test_clock = 1790553600;
static time_t test_time(time_t *value) { if (value) *value = test_clock; return test_clock; }
#define time test_time
#include "../words_service.c"
#undef time

typedef struct { char path[512]; unsigned char *data; size_t size; } file_t;
typedef struct { file_t *file; size_t position; } handle_t;
static file_t files[512];
static handle_t handles[32];
static bool fail_writes;
static bool fail_allocations;
static uint64_t free_space = 64 * 1024 * 1024;
static unsigned allocations;
static epd_service_t service;

int statfs(const char *path, struct statfs *value)
{ (void)path; *value = (struct statfs){(uint32_t)free_space, (uint32_t)free_space, 1}; return 0; }

static file_t *find_file(const char *path, bool create)
{
    file_t *empty = NULL;
    for (unsigned i = 0; i < 512; ++i)
    {
        if (!strcmp(files[i].path, path)) return &files[i];
        if (!empty && !files[i].path[0]) empty = &files[i];
    }
    if (empty && create) { strcpy(empty->path, path); return empty; }
    return NULL;
}
__declspec(dllexport) bool words_test_file(const char *path, const void *data, uint32_t size)
{
    file_t *f = find_file(path, true);
    if (!f) return false;
    unsigned char *p = realloc(f->data, size ? size : 1);
    if (!p) return false;
    f->data = p; f->size = size; memcpy(p, data, size); return true;
}
void *epd_app_alloc(size_t size, int region) { (void)region; void *p = fail_allocations ? NULL : malloc(size); if (p) ++allocations; return p; }
void *epd_app_realloc(void *pointer, size_t size, int region)
{
    (void)region;
    if (fail_allocations) return NULL;
    bool fresh = pointer == NULL;
    void *p = realloc(pointer, size);
    if (p && fresh) ++allocations;
    return p;
}
void epd_app_free(void *pointer) { if (pointer) { --allocations; free(pointer); } }
void storage_lock(void) {}
void storage_unlock(void) {}
bool storage_available(int volume) { (void)volume; return true; }
bool storage_path_available(const char *path) { (void)path; return true; }
bool storage_mkdirs(const char *path) { (void)path; return true; }
bool storage_app_available(const char *id) { (void)id; return true; }
const char *storage_path_root(const char *path) { return !strncmp(path, "/sdcard/", 8) ? "/sdcard" : "/flash"; }
bool storage_file_recover(const char *path) { (void)path; return true; }
bool storage_file_commit(const char *temporary, const char *path) { return test_rename(temporary, path) == 0; }
bool storage_app_path(char *out, size_t size, const char *id, storage_app_area_t area, const char *relative)
{
    static const char *const names[] = {"apps", "data", "cache"};
    if (!out || !size || !id || !relative || (unsigned)area >= 3) return false;
    int n = snprintf(out, size, "/flash/%s/%s%s%s", names[area], id, *relative ? "/" : "", relative);
    return n > 0 && (size_t)n < size;
}
bool storage_info(int volume, storage_info_t *info)
{ (void)volume; *info = (storage_info_t){true, free_space, 0}; return true; }
uint32_t storage_crc32(const void *data, size_t size)
{
    const uint8_t *p = data; uint32_t crc = UINT32_MAX;
    while (size--) { crc ^= *p++; for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u))); }
    return ~crc;
}
bool storage_record_save(const char *path, uint32_t magic, uint32_t version, const void *data, size_t size)
{
    if (fail_writes) return false;
    uint32_t *buffer = malloc(size + 16);
    if (!buffer) return false;
    buffer[0] = magic; buffer[1] = version; buffer[2] = (uint32_t)size; buffer[3] = storage_crc32(data, size);
    memcpy(buffer + 4, data, size);
    bool ok = words_test_file(path, buffer, (uint32_t)size + 16); free(buffer); return ok;
}
bool storage_record_load(const char *path, uint32_t magic, uint32_t version, void *data, size_t size)
{
    file_t *f = find_file(path, false);
    if (!f || f->size != size + 16) return false;
    uint32_t header[4]; memcpy(header, f->data, sizeof(header));
    memcpy(data, f->data + 16, size);
    return header[0] == magic && header[1] == version && header[2] == size && header[3] == storage_crc32(data, size);
}
int test_open(const char *path, int flags, ...)
{
    file_t *f = find_file(path, (flags & O_CREAT) != 0);
    if (!f) return -1;
    if (flags & O_TRUNC) f->size = 0;
    for (int i = 0; i < 32; ++i) if (!handles[i].file) { handles[i] = (handle_t){f, 0}; return i; }
    return -1;
}
int test_read(int fd, void *buffer, size_t size)
{
    if (fd < 0 || fd >= 32 || !handles[fd].file) return -1;
    handle_t *h = &handles[fd];
    if (h->position >= h->file->size) return 0;
    if (size > h->file->size - h->position) size = h->file->size - h->position;
    memcpy(buffer, h->file->data + h->position, size); h->position += size; return (int)size;
}
int test_write(int fd, const void *buffer, size_t size)
{
    if (fail_writes || fd < 0 || fd >= 32 || !handles[fd].file) return -1;
    handle_t *h = &handles[fd]; file_t *f = h->file;
    if (h->position + size > f->size)
    {
        unsigned char *p = realloc(f->data, h->position + size);
        if (!p) return -1;
        f->data = p; f->size = h->position + size;
    }
    memcpy(f->data + h->position, buffer, size); h->position += size; return (int)size;
}
off_t test_lseek(int fd, off_t offset, int whence)
{
    if (fd < 0 || fd >= 32 || !handles[fd].file) return -1;
    off_t value = offset + (whence == SEEK_END ? handles[fd].file->size : whence == SEEK_CUR ? handles[fd].position : 0);
    if (value < 0) return -1;
    handles[fd].position = (size_t)value; return value;
}
int test_close(int fd) { if (fd < 0 || fd >= 32) return -1; handles[fd].file = NULL; return 0; }
int test_fsync(int fd) { (void)fd; return fail_writes ? -1 : 0; }
int test_stat(const char *path, struct test_stat *value)
{
    file_t *f = find_file(path, false);
    if (!f) { errno = -ENOENT; return -1; }
    value->st_size = f->size; value->st_mode = 1; return 0;
}
int test_rename(const char *from, const char *to)
{
    file_t *f = find_file(from, false), *old = find_file(to, false);
    if (!f || fail_writes) return -1;
    if (old && old != f) { free(old->data); memset(old, 0, sizeof(*old)); }
    strcpy(f->path, to); return 0;
}
int test_unlink(const char *path)
{
    file_t *f = find_file(path, false);
    if (!f) return -1;
    free(f->data); memset(f, 0, sizeof(*f)); return 0;
}
DIR *test_opendir(const char *path)
{ DIR *d = calloc(1, sizeof(*d)); if (d) strcpy(d->path, path); return d; }
struct dirent *test_readdir(DIR *dir)
{
    size_t prefix = strlen(dir->path);
    while (dir->index < 512)
    {
        const char *p = files[dir->index++].path;
        if (!strncmp(p, dir->path, prefix) && p[prefix] == '/' && !strchr(p + prefix + 1, '/'))
        { snprintf(dir->entry.d_name, sizeof(dir->entry.d_name), "%s", p + prefix + 1); return &dir->entry; }
    }
    return NULL;
}
int test_closedir(DIR *dir) { free(dir); return 0; }
rt_base_t rt_hw_interrupt_disable(void) { return 0; }
void rt_hw_interrupt_enable(rt_base_t level) { (void)level; }
void epd_service_wake(epd_service_t *s) { (void)s; }
bool epd_service_cancelled(epd_service_t *s) { return s->cancelled; }
void epd_service_wait(epd_service_t *s, int ticks) { (void)s; (void)ticks; }
void rt_thread_mdelay(int milliseconds) { (void)milliseconds; }
void rt_kprintf(const char *format, ...) { (void)format; }
void *dlmodule_find(const char *name) { (void)name; return NULL; }
epd_service_t *app_service_start(const char *id, const epd_background_t *d, void *m)
{ (void)id; (void)d; (void)m; service.cancelled = false; return &service; }
void app_service_stop(const char *id) { (void)id; service.cancelled = true; }

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "CHECK failed at line %d: %s\n", __LINE__, #x); return __LINE__; } } while (0)
static bool perform(words_job_t job, words_result_t *result)
{
    memset(result, 0, sizeof(*result));
    job.serial = ++generation;
    input_t input = {&job, NULL, -1};
    return execute(&input, result, true);
}

__declspec(dllexport) int words_test_learning(void)
{
    CHECK(storage_app_path(book_file, sizeof(book_file), "words", STORAGE_APP_DATA, "book.wdb"));
    CHECK(storage_app_path(book_temp, sizeof(book_temp), "words", STORAGE_APP_DATA, "book.import"));
    words_index_t priority[] = {
        {.phase = WORDS_REVIEW, .due = 100}, {.phase = WORDS_LEARNING, .due = 200},
        {.phase = WORDS_RELEARNING, .due = 150}, {.phase = WORDS_NEW, .due = 0},
        {.phase = WORDS_LEARNING, .due = 50, .flags = WORDS_PAUSED}};
    CHECK(words_due_select(priority, 5, 300) == 2);
    CHECK(words_due_select(priority, 5, 120) == 0);
    CHECK(words_due_select(priority, 5, 10) == -1);
    CHECK(words_store_open()); daily_day = INT32_MIN;
    words_result_t *r = malloc(sizeof(*r)); CHECK(r);
    CHECK(perform((words_job_t){.kind = WORDS_HOME}, r)); CHECK(r->quota == 20 && r->counts.added == 0);
    CHECK(perform((words_job_t){.kind = WORDS_NEXT}, r)); CHECK(r->slot && r->token);
    uint32_t slot = r->slot, token = r->token;
    char original[96]; strcpy(original, r->fields[WORDS_WORD]);
    CHECK(words_store_flush()); words_store_close(); CHECK(words_store_open()); active_slot = 0;
    CHECK(perform((words_job_t){.kind = WORDS_NEXT}, r)); CHECK(r->slot == slot && !strcmp(r->fields[0], original));
    token = r->token;
    CHECK(perform((words_job_t){.kind = WORDS_COLLECT, .slot = slot, .value = 1}, r));
    CHECK(r->flags & WORDS_COLLECTED); CHECK(r->token == token);
    words_job_t rating = {.kind = WORDS_RATE, .slot = slot, .token = token, .value = WORDS_GOOD};
    CHECK(perform(rating, r)); CHECK(r->accepted && r->counts.added == 1 && r->counts.ratings == 1);
    CHECK(!perform(rating, r));
    CHECK(perform((words_job_t){.kind = WORDS_COLLECT, .slot = slot, .value = 0}, r));
    CHECK(!(r->flags & WORDS_COLLECTED));
    words_record_t record; CHECK(words_store_get(slot - 1, &record)); CHECK(record.card.reviews == 1);
    CHECK(perform((words_job_t){.kind = WORDS_PAUSE, .slot = slot, .value = 1}, r));
    CHECK(words_store_index()[slot - 1].flags & WORDS_PAUSED);
    CHECK(perform((words_job_t){.kind = WORDS_PAUSE, .slot = slot, .value = 0}, r));
    words_store_config()->pending = 0; words_store_config_changed();
    CHECK(perform((words_job_t){.kind = WORDS_QUOTA, .value = 0}, r));
    test_clock += 86400;
    CHECK(perform((words_job_t){.kind = WORDS_NEXT}, r)); CHECK(r->slot == slot && r->counts.added == 0);
    token = r->token;
    CHECK(perform((words_job_t){.kind = WORDS_RATE, .slot = slot, .token = token, .value = WORDS_EASY}, r));
    CHECK(r->counts.reviewed == 1 && r->counts.added == 0);
    CHECK(words_store_flush());

    /* A failed block write preserves its dirty contents for a subsequent retry. */
    CHECK(words_store_get(slot - 1, &record)); record.flags |= WORDS_COLLECTED;
    CHECK(words_store_put(slot - 1, &record)); fail_writes = true;
    CHECK(!words_store_flush()); CHECK(words_store_dirty());
    fail_writes = false; CHECK(words_store_flush()); words_store_close(); CHECK(words_store_open());
    CHECK(words_store_get(slot - 1, &record)); CHECK(record.flags & WORDS_COLLECTED);
    CHECK(record.card.reviews == 2);

    /* Cross-book IDs and case preservation. */
    words_job_t book = {.kind = WORDS_BOOK_SELECT};
    strcpy(book.path, "/sdcard/words/books/second.wdb"); strcpy(book.query, "Second");
    free_space = 1; CHECK(!perform(book, r)); CHECK(strcmp(words_store_config()->title, "Second"));
    free_space = 64 * 1024 * 1024; CHECK(perform(book, r)); CHECK(!strcmp(words_store_config()->title, "Second"));
    CHECK(!strcmp(words_store_config()->book, BOOK_FILE));
    CHECK(perform((words_job_t){.kind = WORDS_BOOKS}, r));
    CHECK(r->matches.count == 3 && !strcmp(r->book_paths[1], BOOK_FILE));
    CHECK(words_store_get(slot - 1, &record)); CHECK(record.card.reviews == 2);
    CHECK(perform((words_job_t){.kind = WORDS_QUOTA, .value = 200}, r));
    for (unsigned i = 0; i < 100; ++i)
    {
        CHECK(perform((words_job_t){.kind = WORDS_NEXT}, r));
        if (!r->slot) break;
        uint32_t s = r->slot, t = r->token;
        CHECK(perform((words_job_t){.kind = WORDS_RATE, .slot = s, .token = t, .value = WORDS_EASY}, r));
    }
    CHECK(words_store_count() >= 70);
    CHECK(words_store_find("Apple") >= 0 && words_store_find("apple") >= 0);
    CHECK(words_store_find("Apple") != words_store_find("apple"));
    CHECK(words_store_flush()); words_store_close(); CHECK(words_store_open());
    CHECK(words_store_count() >= 70);
    for (uint32_t i = 0; i < words_store_count(); ++i) CHECK(words_store_content(i, r));

    /* Time rollback never mutates an existing card. */
    CHECK(words_store_get(slot - 1, &record));
    words_record_t untouched = record;
    CHECK(!words_record_rate(&record, WORDS_GOOD, record.card.last_review - 1, daily_day, &record));
    CHECK(!memcmp(&untouched, &record, sizeof(record)));

    /* Removing an unseen collection-only card removes its pending learning task. */
    memset(r, 0, sizeof(*r));
    const char content[] = "outside\0\0meaning\0\0\0\0";
    memcpy(r->content, content, sizeof(content));
    char *field = r->content;
    for (unsigned i = 0; i < WORDS_FIELD_COUNT; ++i) { r->fields[i] = field; field += strlen(field) + 1; }
    int extra = words_store_ensure(r); CHECK(extra >= 0);
    CHECK(perform((words_job_t){.kind = WORDS_COLLECT, .slot = extra + 1, .value = 1}, r));
    CHECK(perform((words_job_t){.kind = WORDS_NEXT}, r)); CHECK(r->slot == (uint32_t)extra + 1);
    token = r->token;
    CHECK(perform((words_job_t){.kind = WORDS_COLLECT, .slot = extra + 1, .token = token, .value = 0}, r));
    CHECK(r->slot == 0);
    CHECK(words_store_get(extra, &record)); CHECK(record.card.phase == WORDS_NEW);

    /* The command queue retains writes after the originating page is gone. */
    worker = &service; stopping = false; queue_count = queue_head = 0;
    uint32_t serial = words_submit(&(words_job_t){.kind = WORDS_QUOTA, .value = 17});
    CHECK(serial); words_cancel(serial); CHECK(queue_count == 1 && jobs[queue_head].value == 17);
    CHECK(!cancellable(jobs[queue_head].kind));
    CHECK(perform(jobs[queue_head], r)); CHECK(words_store_config()->quota == 17);
    queue_count = 0; worker = NULL;
    CHECK(words_store_flush());
    /* Cross the former record/file limit, then fail and retry real growth. */
    const uint32_t target = 9216;
    uint32_t existing = words_store_count();
    for (uint32_t i = existing; i < target; ++i)
    {
        snprintf(record.word, sizeof(record.word), "capacity-%u", i);
        CHECK(words_store_put(i, &record));
    }
    fail_allocations = true;
    CHECK(!words_store_put(target, &record));
    CHECK(words_store_count() == target);
    CHECK(!strcmp(words_store_error(), "内存不足"));
    CHECK(words_store_get(target - 1, &record));
    CHECK(!strcmp(record.word, "capacity-9215"));
    fail_allocations = false;
    strcpy(record.word, "capacity-retry");
    CHECK(words_store_put(target, &record));
    CHECK(words_store_flush()); words_store_close(); CHECK(words_store_open());
    CHECK(words_store_count() == target + 1);
    CHECK(words_store_get(target, &record));
    CHECK(!strcmp(record.word, "capacity-retry"));
    words_store_close(); CHECK(allocations == 0);
    for (unsigned i = 0; i < 32; ++i) CHECK(!handles[i].file);
    free(r);
    return 0;
}
