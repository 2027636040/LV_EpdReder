#include "words_store.h"
#include <dfs_posix.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <errno.h>

static char root[64], config_path[96], content_path[96];
#define CHUNK_RECORDS 32u
#define CHUNK_MAGIC 0x57444331u
#define CONFIG_MAGIC 0x57444346u
typedef struct { uint32_t count; words_record_t records[CHUNK_RECORDS]; } chunk_t;

static words_index_t *index_data;
static chunk_t *cache;
static words_config_t config;
static uint32_t count, capacity, cached_chunk;
static bool dirty_chunk, dirty_config;
static char error[96];

static bool fail(const char *text) { snprintf(error, sizeof(error), "%s", text); return false; }
static void chunk_path(char *path, uint32_t n) { snprintf(path, 96, "%s/states/%03lu.dat", root, (unsigned long)n); }

static bool reserve(uint32_t needed)
{
    if (needed <= capacity) return true;
    /* Slots are returned as int; multiplication must also fit the allocator. */
    if (needed > INT_MAX || needed > SIZE_MAX / sizeof(*index_data)) return fail("学习记录超出索引范围");
    size_t size = needed;
    size_t extra = 128u - needed % 128u;
    if (extra != 128u && extra <= SIZE_MAX / sizeof(*index_data) - size && extra <= INT_MAX - size)
        size += extra;
    words_index_t *next = epd_app_realloc(index_data, size * sizeof(*next), EPD_APP_PSRAM);
    if (!next && size != needed)
    {
        size = needed;
        next = epd_app_realloc(index_data, size * sizeof(*next), EPD_APP_PSRAM);
    }
    if (!next) return fail("内存不足");
    index_data = next; capacity = (uint32_t)size;
    return true;
}

static void index_set(uint32_t slot, const words_record_t *record)
{
    index_data[slot] = (words_index_t){.due = record->card.due, .hash = words_word_hash(record->word),
                                     .phase = record->card.phase, .flags = (uint8_t)record->flags};
}

bool words_store_flush(void)
{
    if ((dirty_chunk || dirty_config) && !storage_app_available("words")) return fail("应用存储不可用");
    if (dirty_chunk)
    {
        char path[96]; chunk_path(path, cached_chunk);
        if (!storage_record_save(path, CHUNK_MAGIC, 1, cache, sizeof(*cache)))
            return fail("保存失败，请检查应用存储");
        dirty_chunk = false;
    }
    if (dirty_config)
    {
        if (!storage_record_save(config_path, CONFIG_MAGIC, 1, &config, sizeof(config)))
            return fail("设置保存失败");
        dirty_config = false;
    }
    error[0] = 0;
    return true;
}

static bool load(uint32_t chunk, bool create)
{
    if (chunk == cached_chunk) return true;
    if (!words_store_flush()) return false;
    char path[96]; chunk_path(path, chunk);
    if (create) memset(cache, 0, sizeof(*cache));
    else if (!storage_record_load(path, CHUNK_MAGIC, 1, cache, sizeof(*cache)))
    { cached_chunk = UINT32_MAX; return fail("学习记录读取失败"); }
    cached_chunk = chunk;
    return true;
}

bool words_store_open(void)
{
    count = capacity = 0; cached_chunk = UINT32_MAX;
    dirty_chunk = dirty_config = false; error[0] = 0;
    char states[96];
    if (!storage_app_path(root, sizeof(root), "words", STORAGE_APP_DATA, "") ||
        !storage_app_path(config_path, sizeof(config_path), "words", STORAGE_APP_DATA, "config.dat") ||
        !storage_app_path(content_path, sizeof(content_path), "words", STORAGE_APP_DATA, "content.bin") ||
        !storage_app_path(states, sizeof(states), "words", STORAGE_APP_DATA, "states")) return fail("应用存储不可用");
    cache = epd_app_alloc(sizeof(*cache), EPD_APP_PSRAM);
    if (!cache) return fail("内存不足");
    memset(&config, 0, sizeof(config));
    storage_app_path(config.book, sizeof(config.book), "words", STORAGE_APP_CODE, "res/library.wdb");
    strcpy(config.title, "内置词书"); config.quota = 20;
    if (!storage_mkdirs(states) || !storage_file_recover(config_path)) return fail("应用存储不可用");
    struct stat st;
    storage_lock(); bool exists = stat(config_path, &st) == 0; storage_unlock();
    if (exists && (!storage_record_load(config_path, CONFIG_MAGIC, 1, &config, sizeof(config)) ||
        !memchr(config.book, 0, sizeof(config.book)) || !memchr(config.title, 0, sizeof(config.title)) ||
        !memchr(config.imported_title, 0, sizeof(config.imported_title)) ||
        config.quota > 200)) return fail("设置文件损坏");
    if (!strcmp(config.book, STORAGE_FLASH_APPS "/words/res/library.wdb") ||
        !strcmp(config.book, STORAGE_SD_APPS "/words/res/library.wdb"))
    {
        char builtin[sizeof(config.book)];
        if (storage_app_path(builtin, sizeof(builtin), "words", STORAGE_APP_CODE, "res/library.wdb") &&
            strcmp(config.book, builtin))
        { strcpy(config.book, builtin); dirty_config = true; }
    }
    if (!strcmp(config.book, "/flash/data/words/book.wdb") || !strcmp(config.book, "/sdcard/.epd/data/words/book.wdb"))
    {
        char imported[sizeof(config.book)];
        if (storage_app_path(imported, sizeof(imported), "words", STORAGE_APP_DATA, "book.wdb") &&
            strcmp(imported, config.book)) { strcpy(config.book, imported); dirty_config = true; }
    }
    for (uint32_t n = 0; ; ++n)
    {
        char path[96]; chunk_path(path, n);
        storage_lock();
        bool recovered = storage_app_available("words") && storage_file_recover(path);
        int status = recovered ? stat(path, &st) : -1;
        int reason = errno;
        storage_unlock();
        if (!recovered) return fail("学习记录读取失败");
        if (status != 0)
        {
            if (reason == -ENOENT) break;
            return fail("学习记录读取失败");
        }
        if (!load(n, false)) return false;
        if (!cache->count || cache->count > CHUNK_RECORDS) return fail("学习记录损坏");
        if (cache->count > UINT32_MAX - count) return fail("学习记录超出索引范围");
        if (!reserve(count + cache->count)) return false;
        for (uint32_t j = 0; j < cache->count; ++j)
        {
            words_record_t *r = &cache->records[j];
            if (!r->word[0] || !memchr(r->word, 0, sizeof(r->word)) || r->card.phase > WORDS_RELEARNING ||
                r->content_size < WORDS_FIELD_COUNT || r->content_size > WORDS_ENTRY_MAX ||
                (r->flags & ~(WORDS_COLLECTED | WORDS_PAUSED)))
                return fail("学习记录损坏");
            index_set(count++, r);
        }
        if (cache->count < CHUNK_RECORDS) break;
    }
    if (config.pending > count) { config.pending = 0; dirty_config = true; }
    return true;
}

void words_store_close(void)
{
    epd_app_free(cache); cache = NULL;
    epd_app_free(index_data); index_data = NULL;
    count = capacity = 0;
}
bool words_store_dirty(void) { return dirty_chunk || dirty_config; }
const char *words_store_error(void) { return error; }
words_config_t *words_store_config(void) { return &config; }
void words_store_config_changed(void) { dirty_config = true; }
uint32_t words_store_count(void) { return count; }
const words_index_t *words_store_index(void) { return index_data; }

bool words_store_get(uint32_t slot, words_record_t *record)
{
    if (slot >= count || !load(slot / CHUNK_RECORDS, false)) return false;
    *record = cache->records[slot % CHUNK_RECORDS]; return true;
}
bool words_store_put(uint32_t slot, const words_record_t *record)
{
    if (slot > count || slot == UINT32_MAX || !reserve(slot + 1) ||
        !load(slot / CHUNK_RECORDS, slot == count && slot % CHUNK_RECORDS == 0)) return false;
    cache->records[slot % CHUNK_RECORDS] = *record;
    if (slot == count) { ++count; ++cache->count; }
    dirty_chunk = true; index_set(slot, record);
    return true;
}

int words_store_find(const char *word)
{
    uint32_t hash = words_word_hash(word);
    for (uint32_t i = 0; i < count; ++i)
    {
        if (index_data[i].hash != hash) continue;
        words_record_t record;
        if (!words_store_get(i, &record)) return -2;
        if (!strcmp(record.word, word)) return (int)i;
    }
    return -1;
}

static bool transfer(int fd, void *data, size_t length, bool writing)
{
    size_t done = 0;
    while (done < length)
    {
        if (!storage_app_available("words")) return false;
        int n = writing ? write(fd, (char *)data + done, length - done) : read(fd, (char *)data + done, length - done);
        if (n <= 0) return false;
        done += n;
    }
    return true;
}

int words_store_ensure(const words_result_t *entry)
{
    int found = words_store_find(entry->fields[WORDS_WORD]);
    if (found != -1) return found;
    if (count == UINT32_MAX || !reserve(count + 1)) return -2;
    words_record_t record = {0};
    strcpy(record.word, entry->fields[WORDS_WORD]);
    record.content_size = (uint32_t)(entry->fields[WORDS_TAGS] + strlen(entry->fields[WORDS_TAGS]) + 1 - entry->content);
    record.content_crc = storage_crc32(entry->content, record.content_size);
    storage_lock();
    int fd = storage_app_available("words") ? open(content_path, O_WRONLY | O_CREAT, 0) : -1;
    off_t offset = fd >= 0 ? lseek(fd, 0, SEEK_END) : -1;
    bool ok = offset >= 0 && (uint64_t)offset + record.content_size < INT32_MAX &&
              transfer(fd, (void *)entry->content, record.content_size, true) && fsync(fd) == 0;
    if (fd >= 0) close(fd);
    storage_unlock();
    if (!ok) { fail("词条保存失败，请检查应用存储"); return -2; }
    record.content_offset = (uint32_t)offset;
    uint32_t slot = count;
    return words_store_put(slot, &record) ? (int)slot : -2;
}

bool words_store_content(uint32_t slot, words_result_t *result)
{
    words_record_t record;
    if (!words_store_get(slot, &record)) return false;
    storage_lock();
    int fd = storage_app_available("words") ? open(content_path, O_RDONLY) : -1;
    bool ok = fd >= 0 && lseek(fd, record.content_offset, SEEK_SET) == (off_t)record.content_offset &&
              transfer(fd, result->content, record.content_size, false);
    if (fd >= 0) close(fd);
    storage_unlock();
    if (!ok || storage_crc32(result->content, record.content_size) != record.content_crc)
        return fail("已保存词条读取失败");
    char *p = result->content, *end = p + record.content_size;
    for (unsigned i = 0; i < WORDS_FIELD_COUNT; ++i)
    {
        result->fields[i] = p;
        char *zero = p < end ? memchr(p, 0, end - p) : NULL;
        if (!zero) return fail("已保存词条损坏");
        p = zero + 1;
    }
    if (p != end || strcmp(record.word, result->fields[WORDS_WORD])) return fail("已保存词条损坏");
    result->slot = slot + 1; result->flags = record.flags;
    return true;
}

bool words_store_counts(int32_t day, int64_t now, words_counts_t *counts)
{
    memset(counts, 0, sizeof(*counts));
    for (uint32_t i = 0; i < count; ++i)
    {
        words_record_t record;
        if (!words_store_get(i, &record)) return false;
        words_count_record(counts, &record, day);
        if (record.card.phase != WORDS_NEW && !(record.flags & WORDS_PAUSED) && record.card.due <= now)
            ++counts->due;
    }
    return true;
}
