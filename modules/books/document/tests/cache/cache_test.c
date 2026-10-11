#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "../../document_cache.h"
#include "../../../formats/formats.h"
#include "../../../formats/book_fs.h"
#include "platform.h"

/* Inspect persisted fields to inject CRC-valid but structurally invalid input. */
typedef struct { uint32_t magic, version, size, crc; } frame_t;
typedef struct {
    uint32_t complete, volume, format, size, modified, conversion;
    uint32_t width, height, end, identity, dependencies, lastuse;
    char source[BOOKSHELF_PATH_MAX], title[BOOKSHELF_NAME_MAX];
} meta_t;
static bookshelf_item_t file(const char *path, uint32_t format)
{
    bookshelf_item_t f = {0};
    snprintf(f.path, sizeof(f.path), "%s", path);
    f.size = 100; f.modified = 42; f.format = format;
    return f;
}
static document_cache_t *create(bookshelf_item_t f)
{ document_cache_t *c = document_cache_create(&f); assert(c); return c; }
static void open_resource(const char *path, bool exists)
{
    bf_FILE *f = bf_fopen(path, "rb");
    assert((f != NULL) == exists);
    if (f) assert(!bf_fclose(f));
}
static bool lookup(document_cache_t *c)
{ char title[BOOKSHELF_NAME_MAX]; uint32_t end; return document_cache_lookup(c, title, sizeof(title), &end); }
static void convert_begin(document_cache_t *c)
{ assert(document_cache_observe_begin(c)); }
static void finish(document_cache_t *c, uint32_t length)
{
    document_cache_observe_end(c);
    assert(document_cache_prepare(c));
    test_sparse(document_cache_binary(c), length);
    assert(document_cache_commit(c, "A title", length));
    assert(document_cache_identity(c));
}
static document_cache_t *complete(bookshelf_item_t f, uint32_t length)
{ document_cache_t *c = create(f); convert_begin(c); finish(c, length); return c; }
static void *payload(document_cache_t *c, frame_t *frame)
{
    storage_lock();
    int fd = open(document_cache_meta(c), O_RDONLY, 0); assert(fd >= 0);
    assert(read(fd, frame, sizeof(*frame)) == sizeof(*frame)); assert(!close(fd));
    void *p = malloc(frame->size); assert(p);
    assert(storage_record_load(document_cache_meta(c), frame->magic, frame->version, p, frame->size));
    storage_unlock(); return p;
}
static void save_payload(document_cache_t *c, const frame_t *frame, void *p)
{ assert(storage_record_save(document_cache_meta(c), frame->magic, frame->version, p, frame->size)); }

static void test_identity_and_offsets(void)
{
    bookshelf_item_t f = file("/flash/identity.epub", BOOK_FORMAT_EPUB);
    strcpy(test_source, f.path); test_source_opens = 0;
    document_cache_t *c = create(f); char original[512]; strcpy(original, document_cache_binary(c));
    assert(!lookup(c)); convert_begin(c); document_cache_observe_end(c);
    assert(!document_cache_commit(c, "early", 0));
    assert(document_cache_prepare(c));
    const uint32_t body[] = {1, 4, 0, 0, 0x73616d65};
    test_put(document_cache_binary(c), body, sizeof(body));
    assert(document_cache_commit(c, "Exact title", sizeof(body)));
    uint32_t identity = document_cache_identity(c);
    char title[128]; uint32_t end = 0;
    assert(document_cache_lookup(c, title, sizeof(title), &end));
    assert(!strcmp(title, "Exact title") && end == sizeof(body));
    assert(!document_cache_lookup(c, title, 3, &end));
    assert(lookup(c) && identity == document_cache_identity(c));
    storage_lock();
    int fd = open(document_cache_binary(c), O_RDONLY, 0); uint32_t actual[5]; assert(fd >= 0);
    assert(read(fd, actual, sizeof(actual)) == sizeof(actual)); close(fd); storage_unlock();
    assert(!memcmp(body, actual, sizeof(body)) && !test_source_opens);
    document_cache_destroy(c); c = create(f); assert(lookup(c));
    assert(document_cache_identity(c) == identity); document_cache_destroy(c);
    ++f.modified; c = create(f); assert(strcmp(original, document_cache_binary(c)) && !lookup(c));
    document_cache_destroy(c); --f.modified; ++f.size; c = create(f);
    assert(strcmp(original, document_cache_binary(c))); document_cache_destroy(c);
    --f.size; strcpy(f.path, "/sdcard/identity.epub"); c = create(f);
    assert(strcmp(original, document_cache_binary(c))); document_cache_destroy(c);
    strcpy(f.path, "/flash/../identity.epub"); assert(!document_cache_create(&f));
    *test_source = 0;
    puts("PASS identity, scan reuse, title/end, byte-zero record offsets, path validation");
}
static void test_late_prepare_and_active(void)
{
    bookshelf_item_t f = file("/flash/late.mobi", BOOK_FORMAT_MOBI);
    document_cache_t *a = complete(f, 24); uint32_t old = document_cache_identity(a);
    document_cache_t *b = create(f); assert(lookup(b));
    convert_begin(a);
    assert(lookup(b) && old == document_cache_identity(b));
    document_cache_observe_end(a);
    assert(!document_cache_prepare(a)); /* b still owns readers of this base. */
    document_cache_destroy(b);
    assert(document_cache_prepare(a));
    b = create(f); assert(!lookup(b)); document_cache_destroy(b);
    test_sparse(document_cache_binary(a), 48);
    assert(document_cache_commit(a, "new", 48));
    assert(lookup(a) && old != document_cache_identity(a));
    b = create(f); assert(lookup(b));
    assert(document_cache_invalidate(a));
    assert(!lookup(b) && !document_cache_identity(b));
    document_cache_t *d = create(f); assert(!lookup(d));
    assert(test_exists(document_cache_binary(a)));
    document_cache_destroy(d); document_cache_destroy(b); document_cache_destroy(a);
    a = create(f); assert(!lookup(a)); document_cache_destroy(a);
    puts("PASS late invalidation, active readers, shared runtime corruption invalidation");
}
static void test_dependencies(void)
{
    bookshelf_item_t f = file("/flash/resources.md", BOOK_FORMAT_MARKDOWN);
    test_put("/flash/assets/present.png", "old", 3);
    document_cache_t *c = create(f); convert_begin(c);
    open_resource("/flash/assets/present.png", true);
    open_resource("/flash/assets/present.png", true);
    struct stat missing;
    assert(bf_stat("/flash/assets/missing.png", &missing) != 0);
    finish(c, 32); assert(lookup(c));
    uint32_t identity = document_cache_identity(c);
    frame_t frame; meta_t *m = payload(c, &frame); assert(m->dependencies == 2); free(m);
    test_put("/flash/assets/present.png", "new", 3); assert(!lookup(c));
    test_put("/flash/assets/present.png", "old", 3); assert(lookup(c));
    test_put("/flash/assets/missing.png", "x", 1); assert(!lookup(c));
    test_remove("/flash/assets/missing.png"); assert(lookup(c));
    test_remove("/flash/assets/present.png"); assert(!lookup(c));
    test_put("/flash/assets/present.png", "new", 3);
    convert_begin(c); open_resource("/flash/assets/present.png", true);
    open_resource("/flash/assets/missing.png", false); finish(c, 32);
    assert(lookup(c) && identity != document_cache_identity(c));
    convert_begin(c); open_resource("/flash/assets/present.png", true);
    test_put("/flash/assets/present.png", "changed during conversion", 25);
    document_cache_observe_end(c); assert(document_cache_prepare(c));
    assert(!document_cache_commit(c, "changed", 32)); assert(!lookup(c));
    document_cache_destroy(c);
    puts("PASS real open observer, missing/added/deleted/modified dependencies and in-flight change");
}
static void test_many_dependencies_and_large_binary(void)
{
    document_cache_t *c = create(file("/flash/many.md", BOOK_FORMAT_MARKDOWN));
    convert_begin(c);
    for (unsigned i = 0; i < 1800; ++i)
    {
        char path[100]; snprintf(path, sizeof(path), "/flash/assets/missing-%04u-resource.png", i);
        open_resource(path, false);
    }
    finish(c, 80u * 1024u * 1024u);
    assert(lookup(c));
    frame_t frame; meta_t *m = payload(c, &frame);
    assert(m->dependencies == 1800 && frame.size > 65536); free(m);
    assert(!document_cache_commit(c, "overflow", UINT32_MAX));
    document_cache_destroy(c);
    bookshelf_item_t f = file("/flash/split", BOOK_FORMAT_SPLIT_EPUB);
    strcpy(test_source, f.path); test_source_opens = 0;
    c = complete(f, 0); assert(lookup(c) && !test_source_opens); document_cache_destroy(c);
    ++f.modified; c = create(f); assert(!lookup(c)); document_cache_destroy(c); *test_source = 0;
    puts("PASS 1800 dependencies, metadata >64 KiB, binary 80 MiB, split scan identity reuse");
}
static void test_malformed_metadata(void)
{
    document_cache_t *c = complete(file("/flash/malformed.epub", BOOK_FORMAT_EPUB), 17);
    frame_t frame; meta_t *m = payload(c, &frame); meta_t saved = *m;
    ++m->modified; save_payload(c, &frame, m); assert(!lookup(c)); *m = saved;
    ++m->conversion; save_payload(c, &frame, m); assert(!lookup(c)); *m = saved;
    ++m->width; save_payload(c, &frame, m); assert(!lookup(c)); *m = saved;
    memset(m->source, 'x', sizeof(m->source)); save_payload(c, &frame, m); assert(!lookup(c)); *m = saved;
    memset(m->title, 'x', sizeof(m->title)); save_payload(c, &frame, m); assert(!lookup(c)); *m = saved;
    m->dependencies = UINT32_MAX; save_payload(c, &frame, m); assert(!lookup(c)); *m = saved;
    ++m->identity; save_payload(c, &frame, m); assert(!lookup(c)); *m = saved;
    save_payload(c, &frame, m); assert(lookup(c));
    test_short_read = true; assert(!lookup(c)); test_short_read = false; assert(lookup(c));
    test_sparse(document_cache_binary(c), 16); assert(!lookup(c)); test_sparse(document_cache_binary(c), 18); assert(!lookup(c));
    test_sparse(document_cache_binary(c), 17); assert(lookup(c));
    frame.size = UINT32_MAX; test_put(document_cache_meta(c), &frame, sizeof(frame)); assert(!lookup(c));
    frame.size = sizeof(*m); save_payload(c, &frame, m); assert(lookup(c));
    storage_lock(); int fd = open(document_cache_meta(c), O_WRONLY, 0); assert(fd >= 0);
    assert(lseek(fd, sizeof(frame) + 100, SEEK_SET) > 0); assert(write(fd, "!", 1) == 1); close(fd); storage_unlock();
    assert(!lookup(c)); free(m); document_cache_destroy(c);
    puts("PASS metadata source/parameter collision checks, malformed lengths/strings/count/CRC, binary length");
}
static void test_faults_and_recovery(void)
{
    test_app_volume = STORAGE_SD;
    bookshelf_item_t f = file("/sdcard/faults.epub", BOOK_FORMAT_EPUB);
    document_cache_t *c = complete(f, 20); convert_begin(c); document_cache_observe_end(c);
    test_fail_sync = true; assert(!document_cache_prepare(c)); test_fail_sync = false;
    assert(lookup(c)); /* Old binary was never replaced. */
    test_fail_write_after = 7; assert(!document_cache_prepare(c)); test_fail_write_after = -1; assert(lookup(c));
    test_short_write = true; assert(document_cache_prepare(c)); test_short_write = false;
    assert(!lookup(c)); test_sparse(document_cache_binary(c), 21);
    test_fail_sync = true; assert(!document_cache_commit(c, "bad", 21)); test_fail_sync = false;
    assert(!lookup(c));
    test_fail_rename_at = 1; assert(!document_cache_commit(c, "rename", 21)); test_fail_rename_at = -1;
    assert(!lookup(c)); /* FAT recovery restores the incomplete generation. */
    assert(document_cache_commit(c, "recovered", 21)); assert(lookup(c));
    test_fail_alloc = 0; assert(!lookup(c)); test_fail_alloc = -1; assert(lookup(c));
    test_fail_sync = true; assert(!document_cache_touch(c)); test_fail_sync = false; assert(lookup(c));
    char backup[520]; snprintf(backup, sizeof(backup), "%s.old", document_cache_meta(c));
    storage_lock(); assert(!rename(document_cache_meta(c), backup)); storage_unlock();
    assert(lookup(c));
    document_cache_destroy(c); test_app_volume = STORAGE_FLASH;
    c = create(file("/flash/allocation.md", BOOK_FORMAT_MARKDOWN)); convert_begin(c);
    test_fail_alloc = 0; open_resource("/flash/assets/no-allocation.png", false); test_fail_alloc = -1;
    document_cache_observe_end(c); assert(document_cache_prepare(c)); test_sparse(document_cache_binary(c), 5);
    assert(!document_cache_commit(c, "allocation failed", 5)); assert(!lookup(c)); document_cache_destroy(c);
    puts("PASS short writes/reads, ENOSPC, fsync/rename/alloc failures, FAT incomplete-marker recovery");
}
static void test_card_sessions(void)
{
    for (unsigned mode = 0; mode < 2; ++mode)
    {
        test_app_volume = mode ? STORAGE_SD : STORAGE_FLASH;
        document_cache_t *c = complete(file(mode ? "/flash/cardcache.epub" : "/sdcard/cardsource.epub", BOOK_FORMAT_EPUB), 9);
        test_hotplug = true; unsigned before = test_io;
        assert(!lookup(c)); assert(!document_cache_prepare(c)); assert(!document_cache_commit(c, "stale", 9));
        assert(!document_cache_reclaim(c)); assert(!document_cache_touch(c));
        document_cache_destroy(c); assert(test_io == before);
    }
    test_app_volume = STORAGE_FLASH;
    document_cache_t *c = create(file("/flash/cross-volume.md", BOOK_FORMAT_MARKDOWN));
    test_put("/sdcard/assets/card.png", "image", 5); convert_begin(c);
    open_resource("/sdcard/assets/card.png", true); finish(c, 8); assert(lookup(c));
    ++test_generation; assert(!lookup(c)); document_cache_destroy(c);
    puts("PASS old source/cache/dependency card sessions cannot reuse or mutate a replacement card");
}
static void test_reclaim_recency(void)
{
    test_now = 1;
    document_cache_t *old = complete(file("/flash/old.epub", BOOK_FORMAT_EPUB), 7);
    char old_bin[512]; strcpy(old_bin, document_cache_binary(old));
    test_now = 2;
    document_cache_t *recent = complete(file("/flash/recent.epub", BOOK_FORMAT_EPUB), 7);
    char recent_bin[512]; strcpy(recent_bin, document_cache_binary(recent));
    test_now = 3; assert(document_cache_touch(old));
    document_cache_t *keep = complete(file("/flash/active.epub", BOOK_FORMAT_EPUB), 7);
    test_put("/flash/old.epub", "original", 8);
    test_put("/flash/cache/books/documents/not-a-cache.bin", "user", 4);
    test_put(document_cache_index(recent), "index", 5);
    char recent_idx[512]; strcpy(recent_idx, document_cache_index(recent));
    document_cache_destroy(old); document_cache_destroy(recent);
    assert(document_cache_reclaim(keep));
    assert(!test_exists(recent_bin) && !test_exists(recent_idx));
    assert(test_exists(old_bin) && lookup(keep));
    assert(test_exists("/flash/old.epub") && test_exists("/flash/cache/books/documents/not-a-cache.bin"));
    document_cache_destroy(keep); test_now = 100;
    puts("PASS persisted recency eviction, active-family protection, source and unrelated-file preservation");
}
static void test_failed_runtime_invalidation(void)
{
    bookshelf_item_t f = file("/flash/bad.epub", BOOK_FORMAT_EPUB);
    document_cache_t *a = complete(f, 4), *b = create(f); assert(lookup(b));
    test_fail_sync = true; assert(!document_cache_invalidate(a)); test_fail_sync = false;
    assert(!lookup(a) && !lookup(b)); document_cache_destroy(a); document_cache_destroy(b);
    a = create(f); assert(!lookup(a)); document_cache_destroy(a);
    puts("PASS failed invalidation quarantines reuse after every active object is destroyed");
}
int main(int argc, char **argv)
{
    assert(argc == 2); test_platform_init(argv[1]);
    test_identity_and_offsets(); test_late_prepare_and_active(); test_dependencies();
    test_many_dependencies_and_large_binary(); test_malformed_metadata(); test_faults_and_recovery();
    test_card_sessions(); test_reclaim_recency(); test_failed_runtime_invalidation();
    assert(!test_allocations);
    puts("PASS all cache tests; zero owned allocations");
    return 0;
}
