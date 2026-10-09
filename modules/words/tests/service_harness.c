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
static bool fail_reads, sd_available = true, app_available = true;
static bool fail_config_stat, drain_worker;
static uint32_t card_session = 1;
static unsigned allocation_calls, fail_allocation_call;
static unsigned read_calls, event_read_call;
enum { READ_EVENT_NONE, READ_EVENT_CANCEL, READ_EVENT_REMOVE, READ_EVENT_REINSERT, READ_EVENT_GENERATION };
static unsigned read_event;
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
static bool allocation_fails(void)
{
    ++allocation_calls;
    return fail_allocations || (fail_allocation_call && allocation_calls == fail_allocation_call);
}
void *epd_app_alloc(size_t size, int region) { (void)region; void *p = allocation_fails() ? NULL : malloc(size); if (p) ++allocations; return p; }
void *epd_app_realloc(void *pointer, size_t size, int region)
{
    (void)region;
    if (allocation_fails()) return NULL;
    bool fresh = pointer == NULL;
    void *p = realloc(pointer, size);
    if (p && fresh) ++allocations;
    return p;
}
void epd_app_free(void *pointer) { if (pointer) { --allocations; free(pointer); } }
void storage_lock(void) {}
void storage_unlock(void) {}
bool storage_available(int volume) { (void)volume; return true; }
bool storage_path_available(const char *path) { return strncmp(path, "/sdcard/", 8) || sd_available; }
uint32_t storage_card_session(void) { return card_session; }
bool storage_session_valid(storage_volume_t volume, uint32_t session)
{ return volume == STORAGE_FLASH || (volume == STORAGE_SD && sd_available && session == card_session); }
bool storage_mkdirs(const char *path) { (void)path; return true; }
bool storage_app_available(const char *id) { (void)id; return app_available; }
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
    if (fail_writes || !app_available) return false;
    uint32_t *buffer = malloc(size + 16);
    if (!buffer) return false;
    buffer[0] = magic; buffer[1] = version; buffer[2] = (uint32_t)size; buffer[3] = storage_crc32(data, size);
    memcpy(buffer + 4, data, size);
    bool ok = words_test_file(path, buffer, (uint32_t)size + 16); free(buffer); return ok;
}
bool storage_record_load(const char *path, uint32_t magic, uint32_t version, void *data, size_t size)
{
    file_t *f = find_file(path, false);
    if (fail_reads || !app_available || !f || f->size != size + 16) return false;
    uint32_t header[4]; memcpy(header, f->data, sizeof(header));
    if (header[0] != magic || header[1] != version || header[2] != size) return false;
    memcpy(data, f->data + 16, size);
    return header[3] == storage_crc32(data, size);
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
    if (fail_reads) return -1;
    if (fd < 0 || fd >= 32 || !handles[fd].file) return -1;
    handle_t *h = &handles[fd];
    if (h->position >= h->file->size) return 0;
    if (size > h->file->size - h->position) size = h->file->size - h->position;
    memcpy(buffer, h->file->data + h->position, size); h->position += size;
    if (++read_calls == event_read_call)
    {
        if (read_event == READ_EVENT_CANCEL) words_cancel(generation);
        if (read_event == READ_EVENT_REMOVE) sd_available = false;
        if (read_event == READ_EVENT_REINSERT) ++card_session;
        if (read_event == READ_EVENT_GENERATION && h->file->size >= 64) h->file->data[40] ^= 1;
    }
    return (int)size;
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
    if (fail_config_stat && strstr(path, "/config.dat")) { errno = -EIO; return -1; }
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
void epd_service_wait(epd_service_t *s, int ticks) { (void)ticks; if (drain_worker) s->cancelled = true; }
void rt_thread_mdelay(int milliseconds) { (void)milliseconds; }
void rt_kprintf(const char *format, ...) { (void)format; }
void *dlmodule_find(const char *name) { (void)name; return NULL; }
epd_service_t *app_service_start(const char *id, const epd_background_t *d, void *m)
{ (void)id; (void)d; (void)m; service.cancelled = false; return &service; }
void app_service_stop(const char *id) { (void)id; service.cancelled = true; }

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "CHECK failed at line %d: %s\n", __LINE__, #x); return __LINE__; } } while (0)
static bool perform(words_job_t job, words_result_t *result)
{
    words_result_clear(result);
    memset(result, 0, sizeof(*result));
    job.serial = ++generation;
    input_t input = {&job, NULL, -1};
    bool ok = execute(&input, result, true);
    input_close(&input);
    return ok;
}

__declspec(dllexport) int words_test_learning(void)
{
    words_index_t priority[] = {
        {.phase = WORDS_REVIEW, .due = 100}, {.phase = WORDS_LEARNING, .due = 200},
        {.phase = WORDS_RELEARNING, .due = 150}, {.phase = WORDS_NEW, .due = 0},
        {.phase = WORDS_LEARNING, .due = 50, .flags = WORDS_PAUSED}};
    CHECK(words_due_select(priority, 5, 300) == 2);
    CHECK(words_due_select(priority, 5, 120) == 0);
    CHECK(words_due_select(priority, 5, 10) == -1);
    CHECK(words_store_open()); daily_day = INT32_MIN;
    words_result_t *r = calloc(1, sizeof(*r)); CHECK(r);
    CHECK(perform((words_job_t){.kind = WORDS_HOME}, r)); CHECK(r->quota == 20 && r->counts.added == 0);
    CHECK(perform((words_job_t){.kind = WORDS_NEXT}, r)); CHECK(r->slot && r->token);
    uint32_t slot = r->slot, token = r->token;
    char original[96]; strcpy(original, r->fields[WORDS_WORD]);
    CHECK(words_store_flush()); words_store_close();
    struct { char book[STORAGE_PATH_MAX], title[96], imported_title[96]; uint8_t identity[24];
             uint32_t cursor, pending, quota; } legacy = {.pending = slot, .quota = 20};
    strcpy(legacy.book, "/sdcard/words/books/old.wdb"); strcpy(legacy.title, "Old book");
    CHECK(storage_record_save("/flash/data/words/config.dat", 0x57444346u, 1, &legacy, sizeof(legacy)));
    CHECK(words_store_open()); CHECK(words_store_config()->pending == slot); active_slot = 0;
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

    /* A reordered WDB2 updates legacy content by exact headword, preserving the record. */
    file_t *new_dictionary = find_file("/fixtures/new.wdb", false); CHECK(new_dictionary);
    CHECK(words_test_file("/flash/apps/words/res/library.wdb", new_dictionary->data, (uint32_t)new_dictionary->size));
    CHECK(perform((words_job_t){.kind = WORDS_DETAIL, .slot = slot}, r));
    CHECK(!strcmp(r->fields[0], original) && r->view.version == 2);
    CHECK(strstr(r->fields[WORDS_TRANSLATION], "苹果"));
    CHECK(perform((words_job_t){.kind = WORDS_SCOPES}, r));
    CHECK(r->matches.count == 2 && !strcmp(r->scope_ids[0], "cet4"));
    words_job_t scope_job = {.kind = WORDS_SCOPE_SELECT};
    strcpy(scope_job.query, "cet6"); memcpy(scope_job.identity, r->identity, sizeof(scope_job.identity));
    words_store_config()->pending = slot;
    CHECK(perform(scope_job, r)); CHECK(!strcmp(words_store_config()->scope_id, "cet6"));
    CHECK(words_store_config()->pending == slot);
    CHECK(perform((words_job_t){.kind = WORDS_NEXT}, r)); CHECK(r->slot == slot);
    words_store_config()->pending = 0;
    CHECK(words_store_get(slot - 1, &record)); record.card.due = test_clock - 8 * 3600 - 1;
    CHECK(words_store_put(slot - 1, &record));
    strcpy(scope_job.query, "cet4"); CHECK(perform(scope_job, r));
    CHECK(perform((words_job_t){.kind = WORDS_NEXT}, r)); CHECK(r->slot == slot);
    CHECK(perform((words_job_t){.kind = WORDS_RATE, .slot = slot, .token = r->token, .value = WORDS_EASY}, r));
    CHECK(words_store_get(slot - 1, &record)); CHECK(record.card.reviews == 3);
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
    words_result_clear(r); memset(r, 0, sizeof(*r));
    const char content[] = "outside\0\0meaning\0\0\0\0";
    CHECK(words_result_reserve(r, sizeof(content) - 1));
    memcpy(r->content, content, sizeof(content) - 1); CHECK(words_result_parse(r));
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
    /* Allocation and filesystem failures never keep a handle or destroy a learned record. */
    fail_allocations = true;
    CHECK(!perform((words_job_t){.kind = WORDS_DETAIL, .slot = slot}, r));
    fail_allocations = false;
    fail_reads = true;
    CHECK(!perform((words_job_t){.kind = WORDS_DETAIL, .slot = slot}, r));
    fail_reads = false;
    CHECK(perform((words_job_t){.kind = WORDS_DETAIL, .slot = slot}, r));
    CHECK(words_store_get(slot - 1, &record)); CHECK(record.card.reviews == 3);
    words_job_t search_job = {.kind = WORDS_SEARCH}; strcpy(search_job.query, "apple");
    strcpy(search_job.path, "/sdcard/words/library.wdb");
    CHECK(words_test_file(search_job.path, new_dictionary->data, (uint32_t)new_dictionary->size));
    CHECK(perform(search_job, r)); CHECK(r->matches.count == 2);
    words_job_t detail_job = {.kind = WORDS_DETAIL, .entry = r->matches.entry[0]};
    strcpy(detail_job.path, search_job.path); memcpy(detail_job.identity, r->identity, sizeof(detail_job.identity));
    sd_available = false; CHECK(!perform(detail_job, r)); sd_available = true;
    CHECK(perform(detail_job, r));
    file_t *sd = find_file(search_job.path, false); sd->data[40] ^= 1;
    CHECK(!perform(detail_job, r)); sd->data[40] ^= 1;
    CHECK(perform(search_job, r));
    worker = &service; service.cancelled = true; CHECK(!words_submit(&search_job));
    service.cancelled = false; worker = NULL;
    app_available = false; CHECK(!perform((words_job_t){.kind = WORDS_DETAIL, .slot = slot}, r)); app_available = true;
    words_result_clear(r);
    words_store_close(); CHECK(allocations == 0);
    for (unsigned i = 0; i < 32; ++i) CHECK(!handles[i].file);
    free(r);
    return 0;
}

static bool no_handles(void)
{
    for (unsigned i = 0; i < 32; ++i) if (handles[i].file) return false;
    return true;
}

static void clear_learning_files(void)
{
    for (unsigned i = 0; i < 512; ++i)
        if (!strncmp(files[i].path, "/flash/data/words/", 18))
        { free(files[i].data); memset(&files[i], 0, sizeof(files[i])); }
    daily_day = INT32_MIN; active_slot = card_token = 0;
}

static bool file_read(void *context, uint32_t offset, void *buffer, size_t size)
{
    file_t *file = context;
    if (offset > file->size || size > file->size - offset) return false;
    memcpy(buffer, file->data + offset, size); return true;
}

static void set_u32(unsigned char *p, uint32_t value)
{
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16); p[3] = (uint8_t)(value >> 24);
}

__declspec(dllexport) int words_test_faults(void)
{
    CHECK(!allocations && no_handles());
    clear_learning_files();
    words_result_t result = {0}; words_result_t *r = &result;
    CHECK(words_store_open()); daily_day = INT32_MIN;

    /* Fail the saved snapshot allocation after committing the first new slot. */
    allocation_calls = 0; fail_allocation_call = 4;
    CHECK(!perform((words_job_t){.kind = WORDS_NEXT}, r));
    CHECK(allocation_calls == 4 && !strcmp(r->error, "内存不足"));
    uint32_t pending = words_store_config()->pending, cursor = words_store_config()->cursor;
    CHECK(pending && no_handles());
    words_record_t record; CHECK(words_store_get(pending - 1, &record));
    CHECK(record.card.phase == WORDS_NEW);
    char word[WORDS_KEY_SIZE]; strcpy(word, record.word);
    fail_allocation_call = 0;
    CHECK(perform((words_job_t){.kind = WORDS_NEXT}, r));
    CHECK(r->slot == pending && !strcmp(r->fields[WORDS_WORD], word));
    CHECK(words_store_config()->cursor == cursor && words_store_count() == 1);
    /* Switching away from the fresh card's scope must retain its ungraded session. */
    CHECK(perform((words_job_t){.kind = WORDS_SCOPES}, r));
    words_job_t scope_job = {.kind = WORDS_SCOPE_SELECT};
    strcpy(scope_job.query, "cet6"); memcpy(scope_job.identity, r->identity, sizeof(scope_job.identity));
    CHECK(perform(scope_job, r)); CHECK(words_store_config()->pending == pending);
    CHECK(perform((words_job_t){.kind = WORDS_NEXT}, r));
    CHECK(r->slot == pending && !strcmp(r->fields[WORDS_WORD], word));
    CHECK(words_store_get(pending - 1, &record)); CHECK(record.card.phase == WORDS_NEW);

    /* Saved bytes, saved display, updated bytes and updated display fail independently. */
    for (unsigned point = 1; point <= 4; ++point)
    {
        allocation_calls = 0; fail_allocation_call = point;
        CHECK(!perform((words_job_t){.kind = WORDS_DETAIL, .slot = pending}, r));
        CHECK(allocation_calls == point && !strcmp(r->error, "内存不足"));
        CHECK(words_store_config()->pending == pending && words_store_count() == 1 && no_handles());
        CHECK(words_store_get(pending - 1, &record)); CHECK(record.card.phase == WORDS_NEW);
    }
    fail_allocation_call = 0;
    CHECK(perform((words_job_t){.kind = WORDS_DETAIL, .slot = pending}, r));
    CHECK(words_store_flush()); words_result_clear(r); words_store_close();
    CHECK(!allocations);

    /* A failed stat must not turn an existing config into fresh defaults. */
    file_t *configuration = find_file("/flash/data/words/config.dat", false); CHECK(configuration);
    uint32_t config_crc = storage_crc32(configuration->data, configuration->size);
    fail_config_stat = true; CHECK(!words_store_open());
    CHECK(!strcmp(words_store_error(), "设置文件读取失败"));
    words_store_close(); fail_config_stat = false;
    CHECK(config_crc == storage_crc32(configuration->data, configuration->size));
    CHECK(!allocations && words_store_open());
    CHECK(words_store_config()->pending == pending);

    /* A rich entry larger than 32 KiB is allocated, persisted and reopened unchanged. */
    words_job_t search = {.kind = WORDS_SEARCH};
    strcpy(search.path, "/fixtures/large.wdb"); strcpy(search.query, "enormous");
    CHECK(perform(search, r)); CHECK(r->matches.count == 1);
    words_job_t detail = {.kind = WORDS_DETAIL, .entry = r->matches.entry[0]};
    strcpy(detail.path, search.path); memcpy(detail.identity, r->identity, sizeof(detail.identity));
    for (unsigned point = 1; point <= 2; ++point)
    {
        allocation_calls = 0; fail_allocation_call = point;
        CHECK(!perform(detail, r)); CHECK(allocation_calls == point);
        CHECK(!strcmp(r->error, "内存不足") && no_handles());
    }
    fail_allocation_call = 0; CHECK(perform(detail, r)); CHECK(r->content_size > 32768);
    int large_slot = words_store_ensure(r); CHECK(large_slot >= 0);
    uint32_t large_size = r->content_size;
    CHECK(words_store_flush()); words_store_close(); CHECK(words_store_open());
    CHECK(words_store_content(large_slot, r)); CHECK(r->content_size == large_size);
    CHECK(!strcmp(r->fields[WORDS_WORD], "enormous"));

    /* Detect corrupt offsets, member identities, payload types and headword/index mismatches. */
    file_t *large = find_file("/fixtures/large.wdb", false); CHECK(large);
    words_dictionary_t dictionary;
    CHECK(words_dictionary_open(&dictionary, file_read, large, (uint32_t)large->size));
    unsigned char saved[8]; memcpy(saved, large->data + dictionary.index + WORDS_KEY_SIZE, 8);
    uint32_t entry_size;
    set_u32(large->data + dictionary.index + WORDS_KEY_SIZE, UINT32_MAX);
    CHECK(!words_dictionary_entry_size(&dictionary, 0, &entry_size));
    memcpy(large->data + dictionary.index + WORDS_KEY_SIZE, saved, 8);
    set_u32(large->data + dictionary.index + WORDS_KEY_SIZE + 4, UINT32_MAX);
    CHECK(!words_dictionary_entry_size(&dictionary, 0, &entry_size));
    memcpy(large->data + dictionary.index + WORDS_KEY_SIZE, saved, 8);
    words_scope_t scope; uint32_t member;
    CHECK(words_dictionary_scope(&dictionary, 0, &scope));
    memcpy(saved, large->data + dictionary.members, 4);
    set_u32(large->data + dictionary.members, UINT32_MAX);
    CHECK(!words_dictionary_member(&dictionary, &scope, 0, &member));
    memcpy(large->data + dictionary.members, saved, 4);
    scope.first = UINT32_MAX; CHECK(!words_dictionary_member(&dictionary, &scope, 0, &member));
    unsigned char prefix = large->data[dictionary.data];
    large->data[dictionary.data] = 1;
    CHECK(!words_dictionary_word(&dictionary, 0, word));
    large->data[dictionary.data] = prefix;
    large->data[dictionary.index] = 'x';
    CHECK(!words_dictionary_word(&dictionary, 0, word));
    CHECK(!perform(detail, r));
    large->data[dictionary.index] = 'e';
    CHECK(perform(detail, r));
    memcpy(saved, r->content + 4, 4); set_u32((unsigned char *)r->content + 4, UINT32_MAX);
    CHECK(!words_result_parse(r)); CHECK(!r->fields[0] && !r->view.word);
    memcpy(r->content + 4, saved, 4); CHECK(words_result_parse(r));
    ((unsigned char *)r->content)[r->content_size - 1] = 0xff; CHECK(!words_result_parse(r));
    words_result_clear(r);

    /* Cancel/remove/reinsert after open; no job may read through an old card session. */
    file_t *fixture = find_file("/fixtures/new.wdb", false); CHECK(fixture);
    strcpy(search.path, "/sdcard/words/library.wdb"); strcpy(search.query, "apple");
    CHECK(words_test_file(search.path, fixture->data, (uint32_t)fixture->size));
    CHECK(perform(search, r));
    detail = (words_job_t){.kind = WORDS_DETAIL, .entry = r->matches.entry[0]};
    strcpy(detail.path, search.path); memcpy(detail.identity, r->identity, sizeof(detail.identity));
    for (unsigned event = READ_EVENT_CANCEL; event <= READ_EVENT_REINSERT; ++event)
    {
        read_calls = 0; event_read_call = 1; read_event = event;
        CHECK(!perform(detail, r)); CHECK(no_handles());
        sd_available = true; event_read_call = read_event = 0;
        CHECK(perform(detail, r));
    }
    /* An update during payload IO invalidates the old generation. */
    read_calls = 0; event_read_call = 5; read_event = READ_EVENT_GENERATION;
    CHECK(!perform(detail, r)); CHECK(no_handles());
    event_read_call = read_event = 0;
    CHECK(!perform(detail, r));
    CHECK(perform(search, r)); memcpy(detail.identity, r->identity, sizeof(detail.identity));
    CHECK(perform(detail, r));
    words_result_clear(r); CHECK(words_store_flush()); words_store_close(); CHECK(!allocations);

    /* Exercise production ownership transitions: submit, cancel, take and worker shutdown. */
    worker = &service; stopping = service.cancelled = false; queue_count = queue_head = 0;
    uint32_t serial = words_submit(&(words_job_t){.kind = WORDS_HOME}); CHECK(serial);
    completed = epd_app_alloc(sizeof(*completed), EPD_APP_PSRAM); CHECK(completed);
    memset(completed, 0, sizeof(*completed)); completed->serial = serial;
    const char snapshot[] = "saved\0\0meaning\0\0\0\0";
    CHECK(words_result_reserve(completed, sizeof(snapshot) - 1));
    memcpy(completed->content, snapshot, sizeof(snapshot) - 1); CHECK(words_result_parse(completed));
    CHECK(allocations == 3); words_cancel(serial); CHECK(!completed && !allocations);
    serial = words_submit(&(words_job_t){.kind = WORDS_HOME}); CHECK(serial);
    completed = epd_app_alloc(sizeof(*completed), EPD_APP_PSRAM); CHECK(completed);
    memset(completed, 0, sizeof(*completed)); completed->serial = serial;
    CHECK(words_result_reserve(completed, sizeof(snapshot) - 1));
    memcpy(completed->content, snapshot, sizeof(snapshot) - 1); CHECK(words_result_parse(completed));
    bool failed; words_result_t *taken = words_take(serial, &failed);
    CHECK(taken && !completed && !failed); CHECK(words_submit(&(words_job_t){.kind = WORDS_HOME}));
    CHECK(!strcmp(taken->fields[0], "saved")); words_result_free(taken); CHECK(!allocations);
    completed = epd_app_alloc(sizeof(*completed), EPD_APP_PSRAM); CHECK(completed);
    memset(completed, 0, sizeof(*completed));
    CHECK(words_submit(&(words_job_t){.kind = WORDS_HOME})); CHECK(!completed && !allocations);
    queue_count = queue_head = 0; worker = NULL;

    /* Run the actual worker loop, not execute(), through its result allocation failure. */
    clear_learning_files(); CHECK(words_service_start());
    serial = words_submit(&(words_job_t){.kind = WORDS_HOME}); CHECK(serial);
    allocation_calls = 0; fail_allocation_call = 2; drain_worker = true;
    run(&service);
    CHECK(!worker && !completed && !allocations && no_handles());
    CHECK(!words_take(serial, &failed) && failed);
    fail_allocation_call = 0;
    CHECK(words_service_start());
    serial = words_submit(&(words_job_t){.kind = WORDS_NEXT}); CHECK(serial);
    run(&service); /* The produced content/display/message remain unclaimed when cancelled. */
    CHECK(!worker && !completed && !allocations && no_handles());
    CHECK(words_store_config()->pending);
    CHECK(words_service_start());
    serial = words_submit(&(words_job_t){.kind = WORDS_QUOTA, .value = 17}); CHECK(serial);
    words_cancel(serial); run(&service);
    CHECK(!allocations && no_handles() && words_store_config()->quota == 17);
    CHECK(words_service_start());
    CHECK(words_submit(&search));
    read_calls = 0; event_read_call = 1; read_event = READ_EVENT_CANCEL;
    run(&service);
    CHECK(!completed && !worker && !allocations && no_handles());
    event_read_call = read_event = 0; drain_worker = false;
    return 0;
}
