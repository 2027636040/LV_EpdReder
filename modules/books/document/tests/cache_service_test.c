/* Real document/cache/storage integration with a fake codec and POSIX host platform. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <string.h>
#include <rthw.h>
#include "../document.h"
#include "../document_cache.h"
#include "../../formats/book_fs.h"
#include "cache/platform.h"

struct epd_service { bool stop; sem_t work, idle; };
static struct epd_service service;
static pthread_t worker;
static bool worker_started;
static unsigned conversions, library_loads, libraries, markup_scopes;
static const char *dependencies[2];
static bool change_card_in_converter, probe_before_commit;
static document_reader_t *precommit_reader;
static char trace[32];
static size_t trace_size;
static rt_tick_t ticks;

static void trace_add(char event)
{
    assert(trace_size + 1 < sizeof(trace));
    trace[trace_size++] = event;
    trace[trace_size] = 0;
}
bool __real_document_cache_observe_begin(document_cache_t *);
bool __wrap_document_cache_observe_begin(document_cache_t *cache)
{ trace_add('B'); return __real_document_cache_observe_begin(cache); }
void __real_document_cache_observe_end(document_cache_t *);
void __wrap_document_cache_observe_end(document_cache_t *cache)
{ trace_add('E'); __real_document_cache_observe_end(cache); }
bool __real_document_cache_prepare(document_cache_t *);
bool __wrap_document_cache_prepare(document_cache_t *cache)
{ trace_add('P'); return __real_document_cache_prepare(cache); }
bool __real_storage_file_commit(const char *, const char *);
bool __wrap_storage_file_commit(const char *from, const char *to)
{
    if (strlen(to) > 4 && !strcmp(to + strlen(to) - 4, ".bin")) trace_add('R');
    return __real_storage_file_commit(from, to);
}
bool __real_document_cache_commit(document_cache_t *, const char *, uint32_t);
bool __wrap_document_cache_commit(document_cache_t *cache, const char *title, uint32_t end)
{ trace_add('C'); return __real_document_cache_commit(cache, title, end); }

rt_tick_t rt_tick_get(void) { return ++ticks; }
rt_tick_t rt_tick_from_millisecond(int milliseconds) { return (rt_tick_t)milliseconds; }
rt_base_t rt_hw_interrupt_disable(void) { return 0; }
void rt_hw_interrupt_enable(rt_base_t state) { (void)state; }
bool epd_service_cancelled(epd_service_t *service) { return service->stop; }
/* Tests explicitly release the waiting worker to control queue boundaries. */
void epd_service_wake(epd_service_t *service) { (void)service; }
static void wait_semaphore(sem_t *semaphore)
{
    int result;
    do result = sem_wait(semaphore); while (result < 0 && errno == EINTR);
    assert(!result);
}
uint32_t epd_service_wait(epd_service_t *service, rt_int32_t timeout)
{
    assert(timeout == RT_WAITING_FOREVER);
    assert(!sem_post(&service->idle));
    wait_semaphore(&service->work);
    return 0;
}
void epd_app_clean_draw_buffer(lv_draw_buf_t *buffer) { (void)buffer; }
bool storage_info(storage_volume_t volume, storage_info_t *info)
{ (void)volume; memset(info, 0, sizeof(*info)); return false; }

static void *worker_entry(void *argument)
{
    document_worker(argument);
    return NULL;
}
static void worker_run(void)
{
    if (!worker_started) {
        memset(&service, 0, sizeof(service));
        assert(!sem_init(&service.work, 0, 0) && !sem_init(&service.idle, 0, 0));
        assert(!pthread_create(&worker, NULL, worker_entry, &service));
        worker_started = true;
    } else assert(!sem_post(&service.work));
    wait_semaphore(&service.idle);
    document_service_process();
}

int book_markup_begin(const book_allocator_t *allocator)
{ assert(allocator && !markup_scopes); ++markup_scopes; return BOOK_OK; }
void book_markup_end(void) { assert(markup_scopes == 1); --markup_scopes; }
int book_markup_convert(const book_sink_t *sink, book_stream_t *stream,
                        const char *base, const book_resources_t *resources)
{ (void)sink; (void)stream; (void)base; (void)resources; abort(); }
book_format_t book_format_detect(const char *path, bool directory)
{ (void)path; (void)directory; return BOOK_FORMAT_MOBI; }
const char *book_format_name(book_format_t format) { (void)format; return "test"; }
int document_image_decode(book_stream_t *stream, bool (*cancelled)(void *), void *user,
                          uint8_t **gray, unsigned *width, unsigned *height)
{ (void)stream; (void)cancelled; (void)user; (void)gray; (void)width; (void)height; abort(); }

int book_format_convert(const char *path, const book_sink_t *sink,
                        const book_allocator_t *allocator, char *error, size_t capacity)
{
    (void)allocator; (void)error; (void)capacity;
    ++conversions;
    assert(!sink->cancelled(sink->user));
    bf_FILE *source = bf_fopen(path, "rb");
    assert(source);
    for (size_t i = 0; i < sizeof(dependencies) / sizeof(dependencies[0]); ++i)
        if (dependencies[i]) {
            bf_FILE *resource = bf_fopen(dependencies[i], "rb");
            if (resource) assert(!bf_fclose(resource));
        }
    int result = sink->metadata(sink->user, "Integration title", "Author");
    if (!result) result = sink->anchor(sink->user, "mobi:0:0", "Chapter", 1);
    char bytes[4096];
    size_t size;
    while (!result && (size = bf_fread(bytes, 1, sizeof(bytes), source)))
        result = sink->text(sink->user, bytes, size, BOOK_STYLE_BOLD);
    assert(!bf_ferror(source) && !bf_fclose(source));
    if (!result) result = sink->paragraph(sink->user);
    if (!result && probe_before_commit) {
        precommit_reader = document_reader_create(sink->user);
        document_record_t record;
        assert(precommit_reader && document_reader_record(precommit_reader, 0, &record));
        assert(record.kind == DOCUMENT_ANCHOR);
    }
    if (change_card_in_converter) ++test_generation;
    return result;
}
static const book_convert_api_t fake_mobi = {
    BOOK_CODEC_ABI, sizeof(book_convert_api_t), book_format_convert
};
void *epd_app_library_open(const char *app, const char *name)
{
    assert(!strcmp(app, "books") && !strcmp(name, "bk_mobi"));
    ++library_loads; ++libraries;
    return (void *)&fake_mobi;
}
void *epd_app_library_symbol(void *library, const char *name)
{ assert(library == &fake_mobi && !strcmp(name, "book_converter")); return library; }
void epd_app_library_close(void *library)
{ assert(library == &fake_mobi && libraries); --libraries; }

typedef struct { char binary[STORAGE_PATH_MAX], meta[STORAGE_PATH_MAX], index[STORAGE_PATH_MAX]; } paths_t;
static paths_t cache_paths(const bookshelf_item_t *file)
{
    document_cache_t *cache = document_cache_create(file);
    assert(cache);
    paths_t paths;
    strcpy(paths.binary, document_cache_binary(cache));
    strcpy(paths.meta, document_cache_meta(cache));
    strcpy(paths.index, document_cache_index(cache));
    document_cache_destroy(cache);
    return paths;
}
static bookshelf_item_t scanned_file(const char *path, book_format_t format)
{
    bookshelf_item_t file = {0};
    struct stat st;
    assert(!bf_stat(path, &st));
    strcpy(file.path, path); strcpy(file.name, "Scanned title");
    file.format = format; file.size = (uint32_t)st.st_size;
    file.modified = (uint32_t)st.st_mtime;
    if (!strcmp(storage_path_root(path), STORAGE_FLASH_ROOT))
        assert(storage_file_signature(path, &file.modified));
    return file;
}
static document_t *open_file(const bookshelf_item_t *file)
{
    char error[128] = {0};
    strcpy(test_source, file->path);
    document_t *document = document_open(file, error, sizeof(error));
    assert(document && !*error);
    return document;
}
static document_status_t complete_status(document_t *document)
{
    document_status_t status;
    document_status(document, &status);
    assert(status.complete && status.stopped && status.result == BOOK_OK && !*status.error);
    assert(!strcmp(status.title, "Integration title"));
    return status;
}
static void close_file(document_t *document)
{
    document_close(document);
    document_service_process();
    assert(!libraries && !markup_scopes);
}
static void idle(void)
{
    if (worker_started) {
        service.stop = true;
        assert(!sem_post(&service.work) && !pthread_join(worker, NULL));
        assert(!sem_destroy(&service.work) && !sem_destroy(&service.idle));
        worker_started = false;
    }
    document_service_shutdown();
    assert(!test_allocations && !libraries && !markup_scopes && !precommit_reader);
}
static void clear_trace(void) { trace_size = 0; trace[0] = 0; }
static document_t *convert_file(const bookshelf_item_t *file)
{
    unsigned before = conversions;
    clear_trace();
    document_t *document = open_file(file);
    document_status_t status;
    document_status(document, &status);
    assert(!status.complete);
    worker_run();
    complete_status(document);
    assert(conversions == before + 1);
    if (strcmp(trace, "BEPRC")) fprintf(stderr, "commit sequence: %s, expected BEPRC\n", trace);
    assert(!strcmp(trace, "BEPRC"));
    return document;
}
static document_t *hit_file(const bookshelf_item_t *file)
{
    unsigned before = conversions, loaded = library_loads, opened = test_source_opens;
    clear_trace();
    document_t *document = open_file(file);
    complete_status(document);
    worker_run();
    assert(conversions == before && library_loads == loaded && !trace_size);
    assert(test_source_opens == opened);
    return document;
}
static void index_missing(document_t *document, uint32_t layout)
{
    void *data = (void *)1; size_t size = 42;
    assert(!document_index_load(document, layout, &data, &size) && !data && !size);
}
static void expect_index(document_t *document, uint32_t layout, const void *data, size_t size)
{
    void *actual = NULL; size_t length = 0;
    assert(document_index_load(document, layout, &actual, &length));
    assert(length == size && !memcmp(data, actual, size));
    epd_app_free(actual);
}

static void test_commit_hit_index(storage_volume_t volume)
{
    test_app_volume = volume;
    const char *path = volume == STORAGE_FLASH ? "/flash/roundtrip.mobi" : "/sdcard/roundtrip.mobi";
    char source[5000];
    for (size_t i = 0; i < sizeof(source); ++i) source[i] = (char)('a' + i % 26);
    test_put(path, source, sizeof(source));
    bookshelf_item_t file = scanned_file(path, BOOK_FORMAT_MOBI);
    paths_t paths = cache_paths(&file);
    probe_before_commit = true;
    document_t *document = convert_file(&file);
    probe_before_commit = false;
    document_status_t status = complete_status(document);
    assert(test_exists(paths.binary) && test_exists(paths.meta));
    document_record_t record;
    assert(document_reader_record(precommit_reader, 0, &record) && record.kind == DOCUMENT_ANCHOR);
    document_reader_destroy(precommit_reader); precommit_reader = NULL;
    unsigned char *records = malloc(status.end), *disk = malloc(status.end);
    assert(records && disk && document_read(document, 0, records, status.end));
    storage_lock();
    int fd = open(paths.binary, O_RDONLY);
    assert(fd >= 0 && read(fd, disk, status.end) == status.end && !close(fd));
    storage_unlock();
    assert(!memcmp(records, disk, status.end));
    document_cursor_t old_position = {sizeof(record) + record.length, 17};
    assert(document_record(document, old_position.record, &record) && record.kind == DOCUMENT_TEXT);
    uint8_t old_byte;
    assert(document_read(document, old_position.record + sizeof(record) + old_position.byte, &old_byte, 1));
    uint8_t index[1024], saved[sizeof(index)];
    for (size_t i = 0; i < sizeof(index); ++i) index[i] = (uint8_t)(i * 13);
    memcpy(saved, index, sizeof(index));
    assert(document_index_save(document, 73, index, sizeof(index)));
    assert(!test_exists(paths.index));
    memset(index, 0, sizeof(index));
    close_file(document);
    assert(test_allocations == 1); /* The queued snapshot outlives the document. */
    worker_run();
    assert(!test_allocations && test_exists(paths.binary) && test_exists(paths.index));
    document = hit_file(&file);
    assert(document_read(document, 0, disk, status.end) && !memcmp(records, disk, status.end));
    uint8_t reopened_byte;
    assert(document_read(document, old_position.record + sizeof(record) + old_position.byte, &reopened_byte, 1));
    assert(old_byte == reopened_byte);
    expect_index(document, 73, saved, sizeof(saved)); index_missing(document, 74);
    close_file(document); idle();
    free(records); free(disk);
    printf("PASS commit/hit/index volume=%u: BEPRC, bytes/old cursor preserved, no source reopen or MOBI reload, async snapshot survives close\n", volume);
}

static void test_source_invalidates(void)
{
    test_app_volume = STORAGE_FLASH;
    test_put("/flash/source.mobi", "old source", 10);
    bookshelf_item_t file = scanned_file("/flash/source.mobi", BOOK_FORMAT_MOBI);
    document_t *document = convert_file(&file); close_file(document);
    document = hit_file(&file); close_file(document);
    uint32_t previous = file.modified;
    test_put(file.path, "new source", 10);
    file = scanned_file(file.path, BOOK_FORMAT_MOBI);
    assert(file.modified != previous);
    document = convert_file(&file); index_missing(document, 73); close_file(document);
    test_put(file.path, "larger changed source", 21);
    file = scanned_file(file.path, BOOK_FORMAT_MOBI);
    document = convert_file(&file); close_file(document);
    document = hit_file(&file); close_file(document); idle();
    puts("PASS source invalidation: scanned flash signature and size changes force conversion");
}

static void test_dependencies_invalidate(void)
{
    test_app_volume = STORAGE_FLASH;
    dependencies[0] = "/flash/assets/present.png";
    dependencies[1] = "/flash/assets/missing.png";
    test_put("/flash/deps.md", "same markdown", 13);
    test_put(dependencies[0], "old", 3);
    bookshelf_item_t file = scanned_file("/flash/deps.md", BOOK_FORMAT_MARKDOWN);
    document_t *document = convert_file(&file);
    const uint8_t index[] = {1, 2, 3};
    assert(document_index_save(document, 73, index, sizeof(index)));
    worker_run(); close_file(document);
    document = hit_file(&file); expect_index(document, 73, index, sizeof(index)); close_file(document);
    test_put(dependencies[0], "new", 3); /* Same length; flash content signature changes. */
    document = convert_file(&file); index_missing(document, 73); close_file(document);
    document = hit_file(&file); close_file(document);
    test_put(dependencies[1], "created", 7);
    document = convert_file(&file); close_file(document);
    document = hit_file(&file); close_file(document);
    test_remove(dependencies[0]);
    document = convert_file(&file); close_file(document);
    document = hit_file(&file); close_file(document); idle();
    dependencies[0] = dependencies[1] = NULL;
    puts("PASS dependency invalidation: changed, created and removed resources force conversion; stale index rejected");
}

static void test_card_invalidates(void)
{
    test_app_volume = STORAGE_SD;
    test_put("/sdcard/card.mobi", "card source", 11);
    bookshelf_item_t file = scanned_file("/sdcard/card.mobi", BOOK_FORMAT_MOBI);
    paths_t paths = cache_paths(&file);
    document_t *document = convert_file(&file);
    const char index[] = "old card index";
    assert(document_index_save(document, 17, index, sizeof(index)));
    ++test_generation;
    uint8_t byte;
    assert(!document_read(document, 0, &byte, 1));
    index_missing(document, 17);
    assert(!document_index_save(document, 17, index, sizeof(index)));
    close_file(document); worker_run();
    assert(!test_exists(paths.index) && test_exists(paths.binary));
    /* A new session may reuse completed portable content from the same card. */
    document = hit_file(&file); close_file(document); idle();
    test_put("/sdcard/removed.mobi", "in progress", 11);
    file = scanned_file("/sdcard/removed.mobi", BOOK_FORMAT_MOBI);
    paths = cache_paths(&file);
    document = open_file(&file);
    change_card_in_converter = true; worker_run(); change_card_in_converter = false;
    document_status_t status; document_status(document, &status);
    assert(status.result == BOOK_CANCELLED && !test_exists(paths.meta));
    close_file(document); idle();
    document = convert_file(&file); close_file(document); idle();
    puts("PASS card invalidation: stale readers/index jobs rejected, interrupted conversion not reusable, remount hit permitted");
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    setvbuf(stdout, NULL, _IONBF, 0);
    test_platform_init(argv[1]);
    test_commit_hit_index(STORAGE_FLASH);
    test_commit_hit_index(STORAGE_SD);
    test_source_invalidates();
    test_dependencies_invalidate();
    test_card_invalidates();
    printf("PASS real document/cache/storage integration: conversions=%u library_loads=%u; zero owned allocations\n",
           conversions, library_loads);
    return 0;
}
