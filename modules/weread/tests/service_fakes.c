/* SPDX-License-Identifier: Apache-2.0 */
/* Included after the real service in service_test.c. All asynchronous events
 * are deterministic hooks; request/result/position state remains production C. */
#include <assert.h>
#include <stdlib.h>
#include <stdarg.h>

struct epd_service { const epd_background_t *definition; bool cancelled, done; };
struct wr_client
{
    wr_cancel_fn cancel;
    void *user;
    bool authenticated, waiting_otp, init_pending;
    char account[32];
};
typedef union { max_align_t align; struct { unsigned magic; } data; } test_allocation_t;
typedef struct
{
    bool used, null_account, null_book;
    storage_app_area_t area;
    char account[32], book[32], name[48];
    unsigned char *data;
    size_t size;
    int64_t content_revision;
} test_record_t;
static struct
{
    epd_service_t service;
    bool mounted, online, authenticated, ui_ready, start_failure, client_failure, store_failure;
    unsigned allocations, live, fail_allocation, lock_depth, waits, wakes, starts, stops;
    unsigned clients, creates, destroys, steps, shutdowns, ui_calls;
    unsigned cover_steps, cover_shutdowns, cover_attempts;
    unsigned shelf_calls, catalog_calls, chapter_calls, login_calls, poll_calls, logout_calls;
    unsigned verify_calls, waiting_getters, log_calls;
    int wait_ticks;
    bool poll_waiting_otp, verify_init_retry;
    char verify_codes[16][5], last_log[256];
    unsigned store_reads, store_writes, session_exports, session_imports;
    unsigned catalog_parses, book_parses;
    unsigned cached_chapter_calls, stored_chapter_calls;
    int64_t cached_revision, stored_revision, catalog_content_revision;
    bool final_flush_observed;
    wr_result_t shelf_result, catalog_result, chapter_result, begin_result, poll_result, verify_result;
    const char *account, *login_account, *shelf_title, *catalog_title, *chapter_text;
    void (*allocation_hook)(void);
    void (*wait_hook)(unsigned step);
    void (*client_hook)(const char *operation, wr_client_t *client);
    void (*shutdown_hook)(void);
    void (*store_hook)(const char *name);
    test_record_t records[32];
} sim;
static unsigned test_cases;

void *epd_app_alloc(size_t size, int region)
{
    assert(region == EPD_APP_PSRAM && size);
    unsigned call = ++sim.allocations;
    if (sim.allocation_hook)
    {
        void (*hook)(void) = sim.allocation_hook;
        sim.allocation_hook = NULL;
        hook();
    }
    if (call == sim.fail_allocation) return NULL;
    test_allocation_t *allocation = malloc(sizeof(*allocation) + size);
    assert(allocation);
    allocation->data.magic = 0xbbee2277;
    ++sim.live;
    return allocation + 1;
}

void epd_app_free(void *pointer)
{
    if (!pointer) return;
    test_allocation_t *allocation = (test_allocation_t *)pointer - 1;
    assert(allocation->data.magic == 0xbbee2277 && sim.live);
    allocation->data.magic = 0;
    --sim.live;
    free(allocation);
}

void rt_kprintf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int size = vsnprintf(sim.last_log, sizeof(sim.last_log), format, args);
    va_end(args);
    assert(size >= 0 && (size_t)size < sizeof(sim.last_log));
    /* Fake credentials and submitted codes must never enter phase logs. */
    assert(!strstr(sim.last_log, "account-A") && !strstr(sim.last_log, "account-B"));
    assert(!strstr(sim.last_log, "login.example"));
    for (unsigned i = 0; i < sim.verify_calls; ++i)
        assert(!strstr(sim.last_log, sim.verify_codes[i]));
    ++sim.log_calls;
}

rt_base_t rt_hw_interrupt_disable(void) { return (rt_base_t)sim.lock_depth++; }
void rt_hw_interrupt_enable(rt_base_t level)
{ assert(sim.lock_depth && (unsigned)level == sim.lock_depth - 1); sim.lock_depth = (unsigned)level; }
bool network_ready(void) { return sim.online; }
bool storage_app_available(const char *id) { assert(!strcmp(id, "weread")); return sim.mounted; }
void *dlmodule_find(const char *name) { assert(!strcmp(name, "weread")); return &sim; }

epd_service_t *app_service_start(const char *id, const epd_background_t *def, void *module)
{
    assert(!strcmp(id, "weread") && module == &sim);
    assert(def->run && def->ui_process && def->network);
    ++sim.starts;
    if (sim.start_failure) return NULL;
    sim.service.definition = def;
    sim.service.cancelled = sim.service.done = false;
    return &sim.service;
}

void app_service_stop(const char *id)
{ assert(!strcmp(id, "weread")); ++sim.stops; sim.service.cancelled = true; }
bool epd_service_cancelled(epd_service_t *service)
{ assert(service == &sim.service); return service->cancelled || !sim.mounted; }
void epd_service_wake(epd_service_t *service) { assert(service == &sim.service); ++sim.wakes; }
uint32_t epd_service_wait(epd_service_t *service, int ticks)
{
    assert(service == &sim.service && !sim.lock_depth);
    assert(ticks == RT_WAITING_FOREVER || ticks == 2000);
    sim.wait_ticks = ticks;
    assert(++sim.waits < 32);
    if (sim.wait_hook) sim.wait_hook(sim.waits);
    else service->cancelled = true;
    return 0;
}

bool epd_reader_worker_step(epd_service_t *service)
{ assert(service == &sim.service); ++sim.steps; return false; }
void epd_reader_worker_shutdown(void)
{ ++sim.shutdowns; if (sim.shutdown_hook) sim.shutdown_hook(); }
bool weread_covers_step(void)
{
    ++sim.cover_steps;
    assert(sim.steps == sim.cover_steps && !sim.lock_depth && !sim.shutdowns);
    if (!sim.cover_attempts) return false;
    --sim.cover_attempts;
    return true;
}
void weread_covers_shutdown(void)
{ assert(stopped && !pending && sim.shutdowns == 1); ++sim.cover_shutdowns; }
bool weread_ui_process(epd_service_t *service, bool stopping)
{
    assert(service == &sim.service);
    if (stopping) assert(service->done);
    ++sim.ui_calls;
    return stopping && sim.ui_ready;
}

static bool test_key_equal(const char *value, const char *key, bool null_key)
{ return value ? !null_key && !strcmp(value, key) : null_key; }
static test_record_t *test_record(storage_app_area_t area, const char *account, const char *book, const char *name)
{
    for (unsigned i = 0; i < 32; ++i)
    {
        test_record_t *record = &sim.records[i];
        if (record->used && record->area == area && !strcmp(record->name, name) &&
            test_key_equal(account, record->account, record->null_account) &&
            test_key_equal(book, record->book, record->null_book)) return record;
    }
    return NULL;
}

static void test_store_seed(storage_app_area_t area, const char *account, const char *book,
                            const char *name, const void *data, size_t size)
{
    test_record_t *record = test_record(area, account, book, name);
    if (!record)
        for (unsigned i = 0; i < 32; ++i) if (!sim.records[i].used) { record = &sim.records[i]; break; }
    assert(record && (!account || strlen(account) < sizeof(record->account)) &&
           (!book || strlen(book) < sizeof(record->book)) && strlen(name) < sizeof(record->name));
    free(record->data);
    memset(record, 0, sizeof(*record));
    record->used = true; record->area = area;
    record->null_account = account == NULL; record->null_book = book == NULL;
    strcpy(record->account, account ? account : ""); strcpy(record->book, book ? book : "");
    strcpy(record->name, name);
    record->data = malloc(size + 1); assert(record->data);
    if (size) memcpy(record->data, data, size);
    record->data[size] = 0; record->size = size;
}

bool weread_store_save(storage_app_area_t area, const char *account, const char *book,
                       const char *name, const void *data, size_t size)
{
    ++sim.store_writes;
    if (sim.store_hook) sim.store_hook(name);
    if (!sim.mounted || sim.store_failure) return false;
    test_store_seed(area, account, book, name, data, size);
    return true;
}

void *weread_store_load(storage_app_area_t area, const char *account, const char *book,
                        const char *name, size_t *size)
{
    ++sim.store_reads; *size = 0;
    test_record_t *record = test_record(area, account, book, name);
    if (!sim.mounted || !record) return NULL;
    void *copy = epd_app_alloc(record->size + 1, EPD_APP_PSRAM);
    if (copy) { memcpy(copy, record->data, record->size + 1); *size = record->size; }
    return copy;
}

bool weread_store_logout(void)
{ return weread_store_save(STORAGE_APP_DATA, NULL, NULL, "session", NULL, 0); }
bool weread_store_chapter(const char *account, const char *book, const char *chapter,
                          int64_t content_revision, bool html, const void *data, size_t size,
                          char path[STORAGE_PATH_MAX], uint32_t *signature)
{
    ++sim.stored_chapter_calls; sim.stored_revision = content_revision;
    if (!weread_store_save(STORAGE_APP_CACHE, account, book, chapter, data, size)) return false;
    test_record(STORAGE_APP_CACHE, account, book, chapter)->content_revision = content_revision;
    snprintf(path, STORAGE_PATH_MAX, "/cache/%s/%s/%s.%s", account, book, chapter, html ? "xhtml" : "txt");
    *signature = 77;
    return true;
}
bool weread_store_cached_chapter(const char *account, const char *book, const char *chapter,
                                 int64_t content_revision, char path[STORAGE_PATH_MAX], uint32_t *signature)
{
    ++sim.cached_chapter_calls; sim.cached_revision = content_revision;
    test_record_t *record = test_record(STORAGE_APP_CACHE, account, book, chapter);
    if (!sim.mounted || !record || record->content_revision != content_revision) return false;
    snprintf(path, STORAGE_PATH_MAX, "/cache/%s/%s/%s.txt", account, book, chapter);
    *signature = 77;
    return true;
}

void wr_books_free(wr_book_t *books, size_t count)
{
    for (size_t i = 0; i < count; ++i)
    {
        epd_app_free(books[i].id); epd_app_free(books[i].title); epd_app_free(books[i].author);
        epd_app_free(books[i].format); epd_app_free(books[i].cover);
    }
    epd_app_free(books);
}
void wr_chapters_free(wr_chapter_t *chapters, size_t count)
{
    for (size_t i = 0; i < count; ++i) { epd_app_free(chapters[i].uid); epd_app_free(chapters[i].title); }
    epd_app_free(chapters);
}
static wr_result_t test_books(const char *title, wr_book_t **books, size_t *count)
{
    *books = NULL; *count = 0;
    wr_book_t *result = epd_app_alloc(sizeof(*result), EPD_APP_PSRAM);
    if (!result) return WR_ERR_MEMORY;
    memset(result, 0, sizeof(*result));
    result->id = weread_string("book"); result->title = weread_string(title);
    if (!result->id || !result->title) { wr_books_free(result, 1); return WR_ERR_MEMORY; }
    *books = result; *count = 1;
    return WR_OK;
}
static wr_result_t test_chapters(const char *title, wr_chapter_t **chapters, size_t *count)
{
    *chapters = NULL; *count = 0;
    wr_chapter_t *result = epd_app_alloc(sizeof(*result), EPD_APP_PSRAM);
    if (!result) return WR_ERR_MEMORY;
    memset(result, 0, sizeof(*result));
    result->uid = weread_string("c1"); result->title = weread_string(title);
    result->update_time = sim.catalog_content_revision;
    if (!result->uid || !result->title) { wr_chapters_free(result, 1); return WR_ERR_MEMORY; }
    *chapters = result; *count = 1;
    return WR_OK;
}
wr_result_t wr_books_parse(const char *json, size_t size, wr_book_t **books, size_t *count)
{
    assert(size == strlen(json)); ++sim.book_parses;
    if (!strcmp(json, "INVALID")) return WR_ERR_PROTOCOL;
    return test_books(json, books, count);
}
wr_result_t wr_chapters_parse(const char *json, size_t size, wr_chapter_t **chapters, size_t *count)
{
    assert(size == strlen(json)); ++sim.catalog_parses;
    if (!strcmp(json, "INVALID")) return WR_ERR_PROTOCOL;
    return test_chapters(json, chapters, count);
}
static wr_result_t test_json(const char *text, char **json, size_t *size)
{
    *json = weread_string(text); *size = *json ? strlen(*json) : 0;
    return *json ? WR_OK : WR_ERR_MEMORY;
}
wr_result_t wr_books_json(const wr_book_t *books, size_t count, char **json, size_t *size)
{ assert(count == 1); return test_json(books[0].title, json, size); }
wr_result_t wr_chapters_json(const wr_chapter_t *chapters, size_t count, char **json, size_t *size)
{ assert(count == 1); return test_json(chapters[0].title, json, size); }

wr_client_t *wr_client_create(wr_cancel_fn cancel, void *user)
{
    ++sim.creates;
    if (sim.client_failure) return NULL;
    wr_client_t *client = epd_app_alloc(sizeof(*client), EPD_APP_PSRAM);
    if (!client) return NULL;
    *client = (wr_client_t){.cancel = cancel, .user = user, .authenticated = sim.authenticated};
    strcpy(client->account, sim.account); ++sim.clients;
    return client;
}
void wr_client_destroy(wr_client_t *client)
{
    if (!client) return;
    assert(sim.clients); --sim.clients; ++sim.destroys;
    epd_app_free(client);
}
void wr_client_set_cancel(wr_client_t *client, wr_cancel_fn cancel, void *user)
{ assert(client); client->cancel = cancel; client->user = user; }
bool wr_client_authenticated(const wr_client_t *client) { return client && client->authenticated; }
const char *wr_client_account_id(const wr_client_t *client) { return client ? client->account : ""; }
const char *wr_client_account_name(const wr_client_t *client) { return client ? client->account : ""; }
void wr_client_logout(wr_client_t *client)
{
    ++sim.logout_calls; client->authenticated = client->waiting_otp = client->init_pending = false;
    client->account[0] = 0;
}
wr_result_t wr_client_session_import(wr_client_t *client, const char *json, size_t size)
{
    ++sim.session_imports;
    if (!size || size >= sizeof(client->account)) return WR_ERR_PROTOCOL;
    memcpy(client->account, json, size); client->account[size] = 0;
    client->authenticated = true;
    return WR_OK;
}
wr_result_t wr_client_session_export(const wr_client_t *client, char **json, size_t *size)
{ ++sim.session_exports; return test_json(client->account, json, size); }
static void test_client_event(const char *operation, wr_client_t *client)
{
    if (client->cancel) (void)client->cancel(client->user);
    if (sim.client_hook) sim.client_hook(operation, client);
}
wr_result_t wr_client_login_begin(wr_client_t *client, char **url)
{
    ++sim.login_calls; *url = NULL;
    client->waiting_otp = client->init_pending = false;
    test_client_event("login", client);
    if (sim.begin_result != WR_OK) return sim.begin_result;
    *url = weread_string("https://login.example/qr");
    return *url ? WR_OK : WR_ERR_MEMORY;
}
wr_result_t wr_client_login_poll(wr_client_t *client)
{
    ++sim.poll_calls;
    test_client_event("poll", client);
    if (sim.poll_result == WR_VERIFY_REQUIRED || sim.poll_waiting_otp) client->waiting_otp = true;
    if (sim.poll_result == WR_OK)
    {
        strcpy(client->account, sim.login_account); client->authenticated = true;
        client->waiting_otp = client->init_pending = false;
    }
    return sim.poll_result;
}
bool wr_client_login_waiting_otp(const wr_client_t *client)
{
    ++sim.waiting_getters;
    return client && (client->waiting_otp || client->init_pending);
}
wr_result_t wr_client_login_verify(wr_client_t *client, const char *code)
{
    assert(client && code && strlen(code) == 4 && sim.verify_calls < 16);
    for (unsigned i = 0; i < 4; ++i) assert(code[i] >= '0' && code[i] <= '9');
    memcpy(sim.verify_codes[sim.verify_calls++], code, 5);
    test_client_event("verify", client);
    if (client->cancel && client->cancel(client->user)) return WR_ERR_CANCELLED;
    assert(client->waiting_otp || client->init_pending);
    if (sim.verify_result == WR_OK)
    {
        strcpy(client->account, sim.login_account); client->authenticated = true;
        client->waiting_otp = client->init_pending = false;
    }
    else if (sim.verify_result == WR_VERIFY_EXPIRED || sim.verify_result == WR_VERIFY_LIMIT)
        client->waiting_otp = client->init_pending = false;
    else if (sim.verify_init_retry)
    { client->waiting_otp = false; client->init_pending = true; }
    return sim.verify_result;
}
wr_result_t wr_client_shelf(wr_client_t *client, wr_book_t **books, size_t *count)
{
    ++sim.shelf_calls; *books = NULL; *count = 0;
    test_client_event("shelf", client);
    return sim.shelf_result == WR_OK ? test_books(sim.shelf_title, books, count) : sim.shelf_result;
}
wr_result_t wr_client_catalog(wr_client_t *client, const char *book, wr_chapter_t **chapters, size_t *count)
{
    assert(book); ++sim.catalog_calls; *chapters = NULL; *count = 0;
    test_client_event("catalog", client);
    return sim.catalog_result == WR_OK ? test_chapters(sim.catalog_title, chapters, count) : sim.catalog_result;
}
wr_result_t wr_client_chapter(wr_client_t *client, const char *book, const char *chapter, char **content, size_t *size)
{
    assert(book && chapter); ++sim.chapter_calls; *content = NULL; *size = 0;
    test_client_event("chapter", client);
    return sim.chapter_result == WR_OK ? test_json(sim.chapter_text, content, size) : sim.chapter_result;
}

static void test_reset(void)
{
    assert(!sim.live && !sim.clients && !sim.lock_depth);
    assert(!worker && !pending && !published && !save_pending);
    for (unsigned i = 0; i < 32; ++i) free(sim.records[i].data);
    memset(&sim, 0, sizeof(sim));
    generation = revision = failed_generation = 0; stopped = false;
    sim.mounted = sim.online = sim.authenticated = sim.ui_ready = true;
    sim.account = "account-A"; sim.login_account = "account-B";
    sim.shelf_title = "remote-shelf"; sim.catalog_title = "remote-catalog";
    sim.chapter_text = "chapter body";
    sim.poll_result = WR_PENDING;
}
static void test_start(void) { assert(weread_start()); assert(worker == &sim.service); }
static void test_run(void)
{
    assert(worker && sim.service.definition);
    sim.service.definition->run(&sim.service);
    sim.service.done = true;
    assert(!sim.clients && !pending && !save_pending && stopped && !sim.lock_depth);
    assert(sim.shutdowns == 1 && sim.cover_shutdowns == 1);
}
static void test_cleanup(void)
{
    assert(sim.service.done);
    sim.ui_ready = true;
    assert(sim.service.definition->ui_process(&sim.service, true));
    assert(!worker && !published && !pending && !save_pending && !sim.live && !sim.lock_depth);
    ++test_cases;
}
