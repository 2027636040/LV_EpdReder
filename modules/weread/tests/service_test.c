/* SPDX-License-Identifier: Apache-2.0 */
#include "service_include/reader_stub.h"
/* Keep the production action enum, public result, request queue, publication,
 * cancellation, fetch, persistence and run/cleanup code in this translation unit. */
#include "../weread_service.c"
#include "../weread_covers.h"
#include "service_fakes.c"

_Static_assert(WR_VERIFY_REQUIRED == 3 && WR_VERIFY_FAILED == 4 &&
               WR_VERIFY_EXPIRED == 5 && WR_VERIFY_LIMIT == 6, "OTP result values changed");

static weread_result_t *test_snapshot(weread_action_t action, wr_result_t status)
{
    weread_result_t *result = weread_result_get();
    assert(result && result->action == action && result->result == status);
    assert(result->generation == generation && result->references >= 2);
    return result;
}

static void test_stop_during_allocation(void) { weread_stop(); }
static void test_cancel_service_during_allocation(void) { sim.service.cancelled = true; }

static weread_result_t test_content(void)
{
    return (weread_result_t){.account = "account-A", .book = "book", .chapter = "c2", .signature = 77};
}
static reader_position_t test_position(unsigned page)
{
    return (reader_position_t){.cursor = {123, 45}, .conversion_version = 2, .layout = 101,
                               .page = page, .pages = 20, .percent = page * 5};
}
static void test_seed_position(const char *account, const char *book, const char *chapter,
                               uint32_t signature, const reader_position_t *position)
{
    size_t size = sizeof(position_record_t) + strlen(chapter) + 1;
    position_record_t *record = calloc(1, size); assert(record);
    record->version = 1; record->signature = signature;
    record->uid_size = (uint32_t)strlen(chapter) + 1; record->position = *position;
    memcpy(record + 1, chapter, record->uid_size);
    test_store_seed(STORAGE_APP_DATA, account, book, "position", record, size);
    free(record);
}

static void test_admission(void)
{
    test_reset();
    assert(!weread_submit(WEREAD_SHELF, NULL, NULL, NULL, NULL));
    sim.start_failure = true; assert(!weread_start() && !worker);
    sim.start_failure = false; test_start();
    unsigned starts = sim.starts; assert(weread_start() && sim.starts == starts);
    weread_stop();
    assert(!weread_start() && weread_stopping());
    assert(!weread_submit(WEREAD_SHELF, NULL, NULL, NULL, NULL));
    weread_result_t content = test_content(); reader_position_t position = test_position(3);
    assert(!weread_save_position(&content, &position));
    test_run(); test_cleanup();

    for (unsigned kind = 0; kind < 2; ++kind)
    {
        for (unsigned save = 0; save < 2; ++save)
        {
            test_reset(); test_start();
            sim.allocation_hook = kind ? test_cancel_service_during_allocation : test_stop_during_allocation;
            if (save) assert(!weread_save_position(&content, &position));
            else assert(!weread_submit(WEREAD_SHELF, NULL, NULL, NULL, NULL));
            assert(!pending && !save_pending && !sim.live && generation == (kind ? 0u : 1u));
            test_run(); test_cleanup();
        }
    }
    test_reset(); test_start(); sim.service.cancelled = true;
    assert(!weread_submit(WEREAD_SHELF, NULL, NULL, NULL, NULL));
    assert(!weread_save_position(&content, &position));
    test_run(); test_cleanup();
    puts("admission: start/stop, allocation-window stop and platform cancellation passed");
}

static uint32_t first_serial, replacement_serial;
static void test_cancel_fetch(const char *operation, wr_client_t *client)
{
    assert(!strcmp(operation, "shelf") && client->cancel && !client->cancel(client->user));
    weread_cancel(first_serial);
    assert(client->cancel(client->user));
}
static void test_replace_fetch(const char *operation, wr_client_t *client)
{
    if (strcmp(operation, "shelf")) return;
    assert(!weread_result_get());
    replacement_serial = weread_submit(WEREAD_CATALOG, "new-book", "new-title", NULL, NULL);
    assert(replacement_serial && replacement_serial != first_serial && client->cancel(client->user));
}
static void test_platform_stop_fetch(const char *operation, wr_client_t *client)
{
    assert(!strcmp(operation, "shelf") && client->cancel && !client->cancel(client->user));
    /* app_service_stop may precede this module's stopped flag. Model a request
     * that completed just as that stop was observed, so publish must recheck. */
    sim.service.cancelled = true;
    assert(!stopped && first_serial == generation);
    assert(weread_cancelled(first_serial) && client->cancel(client->user));
}
static void test_wait_replacement(unsigned step)
{
    if (step == 1) assert(!weread_result_get());
    else
    {
        assert(step == 2);
        weread_result_t *result = test_snapshot(WEREAD_CATALOG, WR_OK);
        assert(result->generation == replacement_serial && !strcmp(result->book, "new-book"));
        weread_result_free(result);
        sim.service.cancelled = true;
    }
}

static void test_generations(void)
{
    test_reset(); test_start();
    first_serial = weread_submit(WEREAD_SHELF, NULL, NULL, NULL, NULL);
    uint32_t latest = weread_submit(WEREAD_CATALOG, "book", "title", NULL, NULL);
    assert(first_serial && latest != first_serial && weread_cancelled(first_serial));
    weread_cancel(first_serial); assert(!weread_cancelled(latest));
    test_run();
    weread_result_t *result = test_snapshot(WEREAD_CATALOG, WR_OK);
    assert(result->generation == latest && !sim.shelf_calls && sim.catalog_calls == 1);
    weread_result_free(result); test_cleanup();

    test_reset(); test_start();
    first_serial = weread_submit(WEREAD_SHELF, NULL, NULL, NULL, NULL);
    sim.client_hook = test_cancel_fetch;
    test_run();
    assert(!weread_result_get() && sim.shelf_calls == 1 && !sim.store_writes && !sim.session_exports);
    test_cleanup();

    test_reset(); test_start();
    first_serial = weread_submit(WEREAD_SHELF, NULL, NULL, NULL, NULL);
    sim.client_hook = test_platform_stop_fetch;
    test_run();
    assert(!published && sim.shelf_calls == 1 && !sim.store_writes && !sim.session_exports);
    test_cleanup();

    test_reset(); test_start();
    first_serial = weread_submit(WEREAD_SHELF, NULL, NULL, NULL, NULL);
    sim.client_hook = test_replace_fetch; sim.wait_hook = test_wait_replacement;
    test_run(); assert(sim.shelf_calls == 1 && sim.catalog_calls == 1);
    assert(!test_record(STORAGE_APP_CACHE, "account-A", NULL, "shelf"));
    test_cleanup();

    test_reset(); test_start(); generation = UINT32_MAX;
    assert(weread_submit(WEREAD_SHELF, NULL, NULL, NULL, NULL) == 1);
    test_run(); test_cleanup();
    puts("generations: queued replacement, in-flight cancellation/replacement, stale publication rejection and wrap passed");
}

static void test_snapshot_lifetime(void)
{
    test_reset(); test_start();
    weread_submit(WEREAD_SHELF, NULL, NULL, NULL, NULL);
    test_run();
    weread_result_t *result = test_snapshot(WEREAD_SHELF, WR_OK);
    weread_result_retain(result);
    assert(result->references == 3);
    sim.ui_ready = false;
    assert(!sim.service.definition->ui_process(&sim.service, true));
    assert(worker && published == result && result->references == 3);
    sim.ui_ready = true;
    assert(sim.service.definition->ui_process(&sim.service, true));
    assert(!worker && !published && result->references == 2);
    assert(!strcmp(result->books[0].title, "remote-shelf"));
    weread_result_free(result); assert(result->references == 1);
    weread_result_free(result);
    assert(!sim.live); ++test_cases;
    puts("lifetime: UI cleanup waits, publication reference, external retains after worker/UI release passed");
}

static void test_accounts_cache(void)
{
    const weread_action_t actions[] = {WEREAD_SHELF, WEREAD_RESTORE, WEREAD_CATALOG, WEREAD_CATALOG_RESTORE};
    for (unsigned i = 0; i < sizeof(actions) / sizeof(actions[0]); ++i)
    {
        test_reset(); sim.online = false; sim.account = "account-B";
        bool shelf = actions[i] == WEREAD_SHELF || actions[i] == WEREAD_RESTORE;
        const char *name = shelf ? "shelf" : "catalog";
        const char *book = shelf ? NULL : "book";
        test_store_seed(STORAGE_APP_CACHE, "account-A", book, name, "cache-A", 7);
        test_store_seed(STORAGE_APP_CACHE, "account-B", book, name, "cache-B", 7);
        test_store_seed(STORAGE_APP_CACHE, "account-B", "other-book", name, "other", 5);
        test_start(); weread_submit(actions[i], book, "title", NULL, NULL); test_run();
        weread_result_t *result = test_snapshot(actions[i], WR_OK);
        assert(result->offline && !strcmp(result->account, "account-B"));
        assert(!strcmp(shelf ? result->books[0].title : result->chapters[0].title, "cache-B"));
        assert(!sim.shelf_calls && !sim.catalog_calls && !sim.store_writes);
        weread_result_free(result); test_cleanup();
    }
    test_reset(); sim.online = false; sim.account = "account-B";
    test_store_seed(STORAGE_APP_CACHE, "account-A", NULL, "shelf", "cache-A", 7);
    test_start(); weread_submit(WEREAD_SHELF, NULL, NULL, NULL, NULL); test_run();
    weread_result_t *result = test_snapshot(WEREAD_SHELF, WR_ERR_HTTP);
    assert(!result->books && !result->book_count); weread_result_free(result); test_cleanup();

    test_reset(); sim.online = false; sim.account = "account-B";
    test_store_seed(STORAGE_APP_DATA, NULL, NULL, "session", "account-A", 9);
    test_store_seed(STORAGE_APP_CACHE, "account-A", NULL, "shelf", "imported-account", 16);
    test_start(); weread_submit(WEREAD_RESTORE, NULL, NULL, NULL, NULL); test_run();
    result = test_snapshot(WEREAD_RESTORE, WR_OK);
    assert(sim.session_imports == 1 && !strcmp(result->account, "account-A"));
    weread_result_free(result); test_cleanup();

    test_reset(); sim.account = ""; sim.authenticated = sim.online = false;
    test_store_seed(STORAGE_APP_DATA, NULL, NULL, "session", NULL, 0);
    test_store_seed(STORAGE_APP_CACHE, "account-A", NULL, "shelf", "cache-A", 7);
    test_start(); weread_submit(WEREAD_RESTORE, NULL, NULL, NULL, NULL); test_run();
    result = test_snapshot(WEREAD_LOGIN, WR_ERR_HTTP);
    assert(!sim.session_imports && !sim.book_parses && !result->books && !result->account[0]);
    weread_result_free(result); test_cleanup();

    test_reset(); sim.catalog_result = WR_ERR_HTTP;
    test_store_seed(STORAGE_APP_CACHE, "account-A", "book", "catalog", "fallback", 8);
    test_start(); weread_submit(WEREAD_CATALOG, "book", "title", NULL, NULL); test_run();
    result = test_snapshot(WEREAD_CATALOG, WR_OK);
    assert(result->offline && sim.catalog_calls == 1 && !strcmp(result->chapters[0].title, "fallback"));
    weread_result_free(result); test_cleanup();
    puts("accounts/cache: account+book isolation, session import, empty logout tombstone and offline fallback passed");
}

static void test_restore_position(void)
{
    test_reset(); test_start();
    test_store_seed(STORAGE_APP_CACHE, "account-A", "book", "catalog", "cached-catalog", 14);
    reader_position_t old = test_position(2), latest = test_position(7);
    test_seed_position("account-A", "book", "c1", 77, &old);
    char account[] = "account-A", book[] = "book", chapter[] = "c2";
    weread_result_t content = {.account = account, .book = book, .chapter = chapter, .signature = 77};
    assert(weread_save_position(&content, &old));
    assert(weread_save_position(&content, &latest));
    account[0] = 'X'; book[0] = 'X'; chapter[0] = 'X';
    weread_submit(WEREAD_CATALOG_RESTORE, "book", "title", NULL, NULL); test_run();
    weread_result_t *result = test_snapshot(WEREAD_CATALOG_RESTORE, WR_OK);
    assert(result->last_chapter && !strcmp(result->last_chapter, "c2"));
    assert(!strcmp(result->chapters[0].title, "cached-catalog") && !sim.catalog_calls);
    test_record_t *record = test_record(STORAGE_APP_DATA, "account-A", "book", "position");
    assert(record && !memcmp(&((position_record_t *)record->data)->position, &latest, sizeof(latest)));
    assert(sim.store_writes == 1);
    weread_result_free(result); test_cleanup();

    for (unsigned mismatch = 0; mismatch < 4; ++mismatch)
    {
        test_reset(); sim.online = false;
        test_store_seed(STORAGE_APP_CACHE, "account-A", "book", "c2", "body", 4);
        test_seed_position(mismatch == 1 ? "account-B" : "account-A", "book",
                           mismatch == 2 ? "c1" : "c2", mismatch == 3 ? 99 : 77, &latest);
        test_start(); weread_submit(WEREAD_CHAPTER, "book", "title", "c2", "chapter"); test_run();
        result = test_snapshot(WEREAD_CHAPTER, WR_OK);
        assert(result->offline && !sim.chapter_calls && strstr(result->path, "/account-A/book/c2.txt"));
        assert(result->restore.page == (mismatch ? 0u : latest.page));
        if (!mismatch) assert(!memcmp(&result->restore, &latest, sizeof(latest)));
        weread_result_free(result); test_cleanup();
    }
    for (unsigned corruption = 0; corruption < 4; ++corruption)
    {
        test_reset();
        test_seed_position("account-A", "book", "c2", 77, &latest);
        record = test_record(STORAGE_APP_DATA, "account-A", "book", "position");
        position_record_t *stored = (position_record_t *)record->data;
        if (corruption == 0) stored->version = 9;
        if (corruption == 1) ++stored->uid_size;
        if (corruption == 2) record->data[record->size - 1] = 'x';
        if (corruption == 3) record->data[sizeof(*stored)] = 0;
        test_start(); weread_submit(WEREAD_CATALOG_RESTORE, "book", "title", NULL, NULL); test_run();
        result = test_snapshot(WEREAD_CATALOG_RESTORE, WR_OK);
        assert(!result->last_chapter && !result->restore.page && sim.catalog_calls == 1);
        weread_result_free(result); test_cleanup();
    }
    puts("positions: owned queue copies/coalescing, catalog restore freshness, account/chapter/signature boundaries and malformed records passed");
}

static weread_result_t test_catalog(wr_chapter_t *chapters, size_t count)
{
    return (weread_result_t){.action = WEREAD_CATALOG, .account = "account-A",
        .book = "book", .title = "book-title", .chapters = chapters, .chapter_count = count};
}

static void test_catalog_revision_wait(unsigned step)
{
    if (step == 1)
    {
        weread_result_t *catalog = test_snapshot(WEREAD_CATALOG, WR_OK);
        assert(catalog->chapter_count == 1 && catalog->chapters[0].update_time == sim.catalog_content_revision);
        assert(catalog->revision != (uint64_t)sim.catalog_content_revision);
        assert(weread_submit_chapter(catalog, 0));
        assert(pending->content_revision == sim.catalog_content_revision);
        weread_result_free(catalog);
    }
    else
    {
        assert(step == 2);
        weread_result_t *content = test_snapshot(WEREAD_CHAPTER, WR_OK);
        assert(!strcmp(content->chapter, "c1") && !strcmp(content->chapter_title, "remote-catalog"));
        assert(sim.cached_revision == sim.catalog_content_revision && sim.stored_revision == sim.catalog_content_revision);
        weread_result_free(content);
        sim.service.cancelled = true;
    }
}

static weread_result_t *replacement_catalog;
static void test_replace_chapter_revision(const char *operation, wr_client_t *client)
{
    assert(!strcmp(operation, "chapter") && !client->cancel(client->user));
    if (sim.chapter_calls != 1) return;
    ++replacement_catalog->chapters[0].update_time;
    replacement_serial = weread_submit_chapter(replacement_catalog, 0);
    assert(replacement_serial && replacement_serial != first_serial && client->cancel(client->user));
}
static void test_wait_chapter_revision(unsigned step)
{
    if (step == 1)
    {
        assert(!published && !sim.stored_chapter_calls);
        assert(sim.cached_revision == replacement_catalog->chapters[0].update_time - 1);
    }
    else
    {
        assert(step == 2);
        weread_result_t *content = test_snapshot(WEREAD_CHAPTER, WR_OK);
        assert(content->generation == replacement_serial && sim.stored_chapter_calls == 1);
        assert(sim.cached_revision == replacement_catalog->chapters[0].update_time);
        assert(sim.stored_revision == sim.cached_revision);
        weread_result_free(content);
        sim.service.cancelled = true;
    }
}

static void test_content_revisions(void)
{
    const int64_t revision64 = INT64_C(4294967303);
    char book[] = "book", title[] = "book-title", uid[] = "c2", chapter_title[] = "chapter-two";
    wr_chapter_t chapters[] = {
        {.uid = "c1", .title = "chapter-one", .update_time = 3},
        {.uid = uid, .title = chapter_title, .update_time = revision64}
    };
    weread_result_t catalog = test_catalog(chapters, 2);
    catalog.book = book; catalog.title = title;
    test_reset();
    assert(!weread_submit_chapter(&catalog, 1));
    test_start();
    assert(!weread_submit_chapter(NULL, 0));
    assert(!weread_submit_chapter(&catalog, 2) && !weread_submit_chapter(&catalog, SIZE_MAX));
    assert(!pending && !generation && !sim.allocations && !sim.wakes);
    uint32_t serial = weread_submit_chapter(&catalog, 1);
    assert(serial && pending->content_revision == revision64);
    request_t *queued = pending;
    unsigned allocations = sim.allocations, wakes = sim.wakes;
    assert(!weread_submit_chapter(&catalog, 2));
    assert(pending == queued && generation == serial && sim.allocations == allocations && sim.wakes == wakes);
    book[0] = title[0] = uid[0] = chapter_title[0] = 'X';
    chapters[1].update_time = 0;
    test_run();
    weread_result_t *result = test_snapshot(WEREAD_CHAPTER, WR_OK);
    assert(!strcmp(result->book, "book") && !strcmp(result->title, "book-title"));
    assert(!strcmp(result->chapter, "c2") && !strcmp(result->chapter_title, "chapter-two"));
    assert(sim.cached_chapter_calls == 1 && sim.stored_chapter_calls == 1);
    assert(sim.cached_revision == revision64 && sim.stored_revision == revision64);
    assert(test_record(STORAGE_APP_CACHE, "account-A", "book", "c2")->content_revision == revision64);
    assert(!weread_submit_chapter(&catalog, 1));
    weread_result_free(result); test_cleanup();

    /* Equal low 32 bits must not make an older revision a cache hit. */
    const int64_t cached_revisions[] = {revision64, revision64 - 1, revision64 + 1, 0, 7};
    for (unsigned online = 0; online < 2; ++online)
    {
        for (unsigned i = 0; i < sizeof(cached_revisions) / sizeof(cached_revisions[0]); ++i)
        {
            test_reset(); sim.online = online != 0;
            wr_chapter_t chapter = {.uid = "c2", .title = "chapter-two", .update_time = revision64};
            catalog = test_catalog(&chapter, 1);
            test_store_seed(STORAGE_APP_CACHE, "account-A", "book", "c2", "cached body", 11);
            test_record_t *record = test_record(STORAGE_APP_CACHE, "account-A", "book", "c2");
            record->content_revision = cached_revisions[i];
            reader_position_t position = test_position(4);
            test_seed_position("account-A", "book", "c2", 77, &position);
            test_start(); assert(weread_submit_chapter(&catalog, 0)); test_run();
            bool match = cached_revisions[i] == revision64;
            result = test_snapshot(WEREAD_CHAPTER, match || online ? WR_OK : WR_ERR_HTTP);
            assert(sim.cached_chapter_calls == 1 && sim.cached_revision == revision64);
            assert(sim.chapter_calls == (unsigned)(!match && online));
            assert(sim.stored_chapter_calls == (unsigned)(!match && online));
            if (!match && online)
            {
                assert(sim.stored_revision == revision64 && record->content_revision == revision64);
                assert(!strcmp((char *)record->data, sim.chapter_text));
            }
            else assert(record->content_revision == cached_revisions[i] && !strcmp((char *)record->data, "cached body"));
            if (!match && !online)
                assert(!result->path[0] && !result->signature && !result->restore.page && !result->last_chapter);
            else
                assert(strstr(result->path, "/account-A/book/c2.txt") && result->offline == !online &&
                       !memcmp(&result->restore, &position, sizeof(position)));
            weread_result_free(result); test_cleanup();
        }
    }

    /* The original five-argument entry point preserves its unknown revision. */
    test_reset(); test_start();
    assert(weread_submit(WEREAD_CHAPTER, "book", "title", "c2", "chapter"));
    assert(pending->content_revision == 0);
    test_run();
    assert(sim.cached_chapter_calls == 1 && sim.cached_revision == 0);
    assert(sim.stored_chapter_calls == 1 && sim.stored_revision == 0);
    test_cleanup();

    test_reset(); sim.catalog_content_revision = INT64_MAX - 17; test_start();
    assert(weread_submit(WEREAD_CATALOG, "book", "title", NULL, NULL));
    sim.wait_hook = test_catalog_revision_wait; test_run();
    assert(sim.catalog_calls == 1 && sim.chapter_calls == 1);
    test_cleanup();

    for (unsigned in_flight = 0; in_flight < 2; ++in_flight)
    {
        test_reset(); test_start();
        wr_chapter_t chapter = {.uid = "c2", .title = "chapter-two", .update_time = revision64};
        catalog = test_catalog(&chapter, 1);
        first_serial = weread_submit_chapter(&catalog, 0);
        if (in_flight)
        {
            replacement_catalog = &catalog;
            sim.client_hook = test_replace_chapter_revision;
            sim.wait_hook = test_wait_chapter_revision;
        }
        else
        {
            ++chapter.update_time;
            replacement_serial = weread_submit_chapter(&catalog, 0);
            assert(replacement_serial != first_serial && weread_cancelled(first_serial));
        }
        test_run();
        assert(sim.chapter_calls == 1 + in_flight && sim.stored_chapter_calls == 1);
        assert(sim.cached_revision == revision64 + 1 && sim.stored_revision == revision64 + 1);
        result = test_snapshot(WEREAD_CHAPTER, WR_OK);
        assert(result->generation == replacement_serial);
        weread_result_free(result); test_cleanup();
        replacement_catalog = NULL;
    }
    puts("content revisions: selected catalog update_time, 64-bit exact cache keys, offline mismatch, owned requests, legacy submit and replacement passed");
}

static void test_final_flush(const char *name)
{
    if (strcmp(name, "position")) return;
    assert(stopped && !pending);
    weread_result_t content = test_content(); reader_position_t position = test_position(8);
    assert(!weread_submit(WEREAD_SHELF, NULL, NULL, NULL, NULL));
    assert(!weread_save_position(&content, &position));
    sim.final_flush_observed = true;
}
static void test_shutdown_admission(void)
{
    assert(stopped && !pending && !save_pending && sim.final_flush_observed);
    assert(!weread_submit(WEREAD_SHELF, NULL, NULL, NULL, NULL));
}
static void test_queue_then_stop(unsigned step)
{
    assert(step == 1);
    weread_result_t content = test_content(); reader_position_t position = test_position(8);
    assert(weread_save_position(&content, &position));
    assert(weread_submit(WEREAD_SHELF, NULL, NULL, NULL, NULL));
    assert(!stopped);
    sim.service.cancelled = true;
}
static void test_exit_drain(void)
{
    test_reset(); test_start();
    sim.wait_hook = test_queue_then_stop; sim.store_hook = test_final_flush;
    sim.shutdown_hook = test_shutdown_admission;
    test_run();
    assert(sim.final_flush_observed && !sim.shelf_calls && !published);
    assert(test_record(STORAGE_APP_DATA, "account-A", "book", "position"));
    test_cleanup();

    test_reset(); test_start();
    weread_result_t content = test_content(); reader_position_t position = test_position(8);
    assert(weread_save_position(&content, &position));
    assert(weread_submit(WEREAD_SHELF, NULL, NULL, NULL, NULL));
    weread_stop(); sim.store_failure = true;
    test_run(); assert(!sim.shelf_calls && sim.store_writes == 1); test_cleanup();
    puts("shutdown: close admission before final flush, discard pending fetch, drain/free position even on storage failure passed");
}

static void test_cover_worker_lifecycle(void)
{
    test_reset(); test_start(); sim.cover_attempts = 2;
    test_run();
    assert(sim.cover_steps == 3 && sim.steps == 3 && sim.waits == 1 && !sim.cover_attempts);
    assert(!published && !sim.shelf_calls && !sim.catalog_calls && !sim.chapter_calls);
    test_cleanup();
    puts("covers: real worker continues after an attempted cover, then waits; reader shuts down before covers after admission closes");
}

static weread_result_t *login_snapshot;
static void test_login_wait(unsigned step)
{
    if (step == 1)
    {
        login_snapshot = test_snapshot(WEREAD_LOGIN, WR_PENDING);
        assert(login_snapshot->qr && !sim.poll_calls);
    }
    else if (step == 2)
    {
        assert(sim.poll_calls == 1 && published == login_snapshot);
        sim.poll_result = WR_OK;
    }
    else
    {
        assert(step == 3 && sim.poll_calls == 2);
        weread_result_t *result = test_snapshot(WEREAD_SHELF, WR_OK);
        assert(!strcmp(result->account, "account-B") && result->revision > login_snapshot->revision);
        assert(login_snapshot->references == 1 && !strcmp(login_snapshot->qr, "https://login.example/qr"));
        weread_result_free(result); weread_result_free(login_snapshot); login_snapshot = NULL;
        sim.service.cancelled = true;
    }
}
static void test_cancel_login_wait(unsigned step)
{
    if (step == 1) { weread_cancel(generation); return; }
    assert(step == 2 && !sim.poll_calls);
    sim.service.cancelled = true;
}
static void test_permission_login_wait(unsigned step)
{
    if (step == 1)
    {
        assert(!sim.poll_calls);
        sim.poll_result = WR_ERR_PERMISSION;
        return;
    }
    assert(step == 2 && sim.poll_calls == 1);
    weread_result_t *result = test_snapshot(WEREAD_LOGIN, WR_ERR_PERMISSION);
    assert(strstr(result->error, "登录请求被拒绝") && !strstr(result->error, "额外验证") &&
           !strstr(result->error, "本章") && !result->verification);
    assert(!sim.shelf_calls && !sim.session_exports);
    weread_result_free(result);
    sim.service.cancelled = true;
}
static void test_login_logout(void)
{
    test_reset(); sim.authenticated = false; sim.account = "";
    test_start(); weread_submit(WEREAD_LOGIN, NULL, NULL, NULL, NULL);
    sim.wait_hook = test_login_wait; test_run();
    assert(sim.login_calls == 1 && sim.shelf_calls == 1);
    test_record_t *session = test_record(STORAGE_APP_DATA, NULL, NULL, "session");
    assert(session && !strcmp((char *)session->data, "account-B"));
    assert(test_record(STORAGE_APP_CACHE, "account-B", NULL, "shelf"));
    assert(!test_record(STORAGE_APP_CACHE, "account-A", NULL, "shelf"));
    test_cleanup();

    test_reset(); sim.authenticated = false; sim.account = "";
    test_start(); weread_submit(WEREAD_LOGIN, NULL, NULL, NULL, NULL);
    sim.wait_hook = test_cancel_login_wait; test_run();
    assert(!sim.poll_calls && !sim.session_exports); test_cleanup();

    test_reset(); sim.authenticated = false; sim.account = "";
    test_start(); weread_submit(WEREAD_LOGIN, NULL, NULL, NULL, NULL);
    sim.wait_hook = test_permission_login_wait; test_run(); test_cleanup();

    test_reset(); sim.chapter_result = WR_ERR_PERMISSION;
    test_start(); weread_submit(WEREAD_CHAPTER, "book", "title", "c2", "chapter"); test_run();
    weread_result_t *permission = test_snapshot(WEREAD_CHAPTER, WR_ERR_PERMISSION);
    assert(strstr(permission->error, "本章") && !strstr(permission->error, "额外验证"));
    assert(!permission->path[0] && !sim.stored_chapter_calls);
    weread_result_free(permission); test_cleanup();

    for (unsigned failure = 0; failure < 2; ++failure)
    {
        test_reset();
        test_store_seed(STORAGE_APP_DATA, NULL, NULL, "session", "account-A", 9);
        sim.store_failure = failure != 0;
        test_start(); weread_submit(WEREAD_LOGOUT, NULL, NULL, NULL, NULL); test_run();
        session = test_record(STORAGE_APP_DATA, NULL, NULL, "session");
        assert(session && session->size == (failure ? 9u : 0u));
        assert(sim.logout_calls == (failure ? 0u : 1u) && sim.login_calls == (failure ? 0u : 1u));
        weread_result_t *result = test_snapshot(failure ? WEREAD_LOGOUT : WEREAD_LOGIN,
                                               failure ? WR_ERR_REMOTE : WR_PENDING);
        weread_result_free(result); test_cleanup();
    }
    puts("login/logout: pending polling, account transition, cancelled poll lifetime, permission messages and durable logout failure passed");
}

static struct
{
    uint32_t login, submitted, replacement;
    unsigned phase, mode, event, fail_offset;
    bool invalidated, offline, init_retry;
    wr_result_t outcome;
    weread_result_t *held;
} otp;

static void test_otp_start(void (*wait_hook)(unsigned))
{
    test_reset(); memset(&otp, 0, sizeof(otp));
    sim.authenticated = false; sim.account = "";
    sim.poll_result = WR_VERIFY_REQUIRED;
    test_start();
    otp.login = weread_submit(WEREAD_LOGIN, NULL, NULL, NULL, NULL);
    assert(otp.login);
    sim.wait_hook = wait_hook;
}

/* The first wait is QR polling; subsequent checks use observable state, not
 * worker iteration counts (VERIFY takes a continue before worker steps). */
static bool test_otp_qr_wait(void)
{
    assert(!sim.ui_calls);
    if (!sim.poll_calls)
    {
        assert(sim.wait_ticks == 2000 && sim.login_calls == 1);
        weread_result_t *qr = test_snapshot(WEREAD_LOGIN, WR_PENDING);
        assert(qr->qr && !qr->verification);
        assert(!weread_submit_code(otp.login, "0012"));
        assert(!pending && generation == otp.login);
        weread_result_free(qr);
        return true;
    }
    assert(sim.poll_calls == 1 && sim.wait_ticks == RT_WAITING_FOREVER);
    return false;
}

static weread_result_t *test_otp_snapshot(wr_result_t status, bool waiting)
{
    weread_result_t *result = test_snapshot(WEREAD_LOGIN, status);
    assert(result->verification == waiting && !result->qr);
    assert(!sim.shelf_calls && !sim.session_exports && !sim.store_writes);
    return result;
}

static uint32_t test_otp_submit(uint32_t from, const char *code)
{
    uint32_t next = weread_submit_code(from, code);
    assert(next && next != from && pending && pending->generation == next);
    assert(pending->action == WEREAD_VERIFY && !strcmp(pending->otp, code));
    request_t *queued = pending;
    unsigned live = sim.live, wakes = sim.wakes;
    /* Both replaying the source token and guessing the queued token fail. */
    assert(!weread_submit_code(from, code) && !weread_submit_code(next, code));
    assert(pending == queued && generation == next && sim.live == live && sim.wakes == wakes);
    return next;
}

static void test_otp_saved_before_shelf(const char *operation, wr_client_t *client)
{
    if (strcmp(operation, "shelf")) return;
    assert(client->authenticated && !wr_client_login_waiting_otp(client));
    test_record_t *session = test_record(STORAGE_APP_DATA, NULL, NULL, "session");
    assert(session && !strcmp((char *)session->data, "account-B"));
}

static void test_otp_success(void)
{
    weread_result_t *result = test_snapshot(WEREAD_SHELF, WR_OK);
    assert(result->generation == otp.submitted && !result->verification && !result->qr);
    assert(!strcmp(result->account, "account-B") && result->book_count == 1);
    assert(sim.shelf_calls == 1 && sim.session_exports && sim.poll_calls == 1);
    assert(test_record(STORAGE_APP_CACHE, "account-B", NULL, "shelf"));
    assert(!test_record(STORAGE_APP_CACHE, "account-A", NULL, "shelf"));
    assert(!weread_submit_code(result->generation, "0012"));
    weread_result_free(result);
    sim.service.cancelled = true;
}

static void test_otp_idle_wait(unsigned step)
{
    (void)step;
    if (test_otp_qr_wait()) return;
    weread_result_t *result = test_otp_snapshot(WR_VERIFY_REQUIRED, true);
    assert(result->generation == otp.login && result->error[0] && !sim.verify_calls);
    const char *invalid[] = {NULL, "", "1", "123", "12345", "+123", " 123", "123 ",
                            "12a4", "12\n4", "12\0003", "\x80" "123", "１２３４", "١٢"};
    unsigned live = sim.live, wakes = sim.wakes;
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        assert(!weread_submit_code(otp.login, invalid[i]));
    assert(!weread_submit_code(0, "0012") && !weread_submit_code(otp.login + 1, "0012"));
    assert(!weread_submit(WEREAD_VERIFY, NULL, NULL, NULL, NULL));
    assert(!pending && generation == otp.login && sim.live == live && sim.wakes == wakes);
    weread_result_free(result);
    if (++otp.phase == 3) sim.service.cancelled = true;
    else weread_wake();
}

static void test_otp_wrong_wait(unsigned step)
{
    (void)step;
    if (test_otp_qr_wait()) return;
    if (otp.phase == 0)
    {
        otp.held = test_otp_snapshot(WR_VERIFY_REQUIRED, true);
        sim.verify_result = WR_VERIFY_FAILED;
        char code[] = "0012";
        otp.submitted = test_otp_submit(otp.login, code);
        memset(code, '9', 4);
    }
    else if (otp.phase == 1)
    {
        weread_result_t *result = test_otp_snapshot(WR_VERIFY_FAILED, true);
        assert(sim.verify_calls == 1 && !strcmp(sim.verify_codes[0], "0012"));
        assert(result->generation == otp.submitted && strstr(result->error, "验证码错误"));
        assert(otp.held->references == 1 && otp.held->verification && otp.held->generation == otp.login);
        weread_result_free(otp.held); otp.held = NULL;
        sim.verify_result = WR_OK;
        otp.submitted = test_otp_submit(result->generation, "0000");
        weread_result_free(result);
    }
    else
    {
        assert(otp.phase == 2 && sim.verify_calls == 2 && !strcmp(sim.verify_codes[1], "0000"));
        test_otp_success();
    }
    ++otp.phase;
}

static void test_otp_retry_wait(unsigned step)
{
    (void)step;
    if (test_otp_qr_wait()) return;
    if (otp.phase == 0)
    {
        weread_result_t *result = test_otp_snapshot(sim.poll_result, true);
        sim.online = !otp.offline;
        sim.verify_result = otp.outcome; sim.verify_init_retry = otp.init_retry;
        otp.submitted = test_otp_submit(result->generation, "0012");
        weread_result_free(result);
    }
    else if (otp.phase == 1)
    {
        weread_result_t *result = test_otp_snapshot(otp.offline ? WR_ERR_HTTP : otp.outcome, true);
        assert(sim.verify_calls == (otp.offline ? 0u : 1u) && result->generation == otp.submitted);
        assert(result->error[0]);
        sim.online = true; sim.verify_result = WR_OK;
        otp.submitted = test_otp_submit(result->generation, "0099");
        weread_result_free(result);
    }
    else
    {
        assert(otp.phase == 2 && sim.verify_calls == (otp.offline ? 1u : 2u));
        assert(!strcmp(sim.verify_codes[sim.verify_calls - 1], "0099"));
        test_otp_success();
    }
    ++otp.phase;
}

static void test_otp_terminal_wait(unsigned step)
{
    (void)step;
    if (test_otp_qr_wait()) return;
    if (!otp.phase)
    {
        weread_result_t *result = test_otp_snapshot(WR_VERIFY_REQUIRED, true);
        sim.verify_result = otp.outcome;
        otp.submitted = test_otp_submit(result->generation, "0012");
        weread_result_free(result);
    }
    else
    {
        weread_result_t *result = test_otp_snapshot(otp.outcome, false);
        assert(result->generation == otp.submitted && sim.verify_calls == 1);
        assert(strstr(result->error, otp.outcome == WR_VERIFY_EXPIRED ? "过期" : "上限"));
        assert(!weread_submit_code(otp.login, "0012") && !weread_submit_code(result->generation, "0012"));
        assert(!pending);
        weread_result_free(result);
        if (otp.phase == 2) sim.service.cancelled = true;
    }
    ++otp.phase;
}

enum { OTP_CANCEL, OTP_STOP, OTP_PLATFORM_STOP, OTP_REMOVE_CARD, OTP_NEW_QR, OTP_EVENTS };
enum { OTP_IDLE, OTP_ALLOCATING, OTP_QUEUED, OTP_IN_FLIGHT, OTP_RESULT_ALLOCATING, OTP_MODES };
static void test_otp_invalidate(void)
{
    assert(!otp.invalidated);
    otp.invalidated = true;
    switch (otp.event)
    {
    case OTP_CANCEL: weread_cancel(generation); break;
    case OTP_STOP: weread_stop(); break;
    case OTP_PLATFORM_STOP: sim.service.cancelled = true; assert(!stopped); break;
    case OTP_REMOVE_CARD: sim.mounted = false; assert(!stopped); break;
    case OTP_NEW_QR:
        sim.poll_result = WR_PENDING;
        otp.replacement = weread_submit(WEREAD_LOGIN, NULL, NULL, NULL, NULL);
        assert(otp.replacement);
        break;
    default: assert(false);
    }
    assert(weread_cancelled(otp.login));
    assert(!weread_submit_code(otp.login, "0012"));
}

static void test_otp_invalidate_client(const char *operation, wr_client_t *client)
{
    if (strcmp(operation, "verify")) return;
    assert(!client->cancel(client->user));
    if (otp.mode == OTP_RESULT_ALLOCATING)
        sim.allocation_hook = test_otp_invalidate;
    else
    {
        assert(otp.mode == OTP_IN_FLIGHT);
        test_otp_invalidate();
        assert(client->cancel(client->user));
    }
}

static void test_otp_invalidation_wait(unsigned step)
{
    (void)step;
    if (!otp.phase)
    {
        if (test_otp_qr_wait()) return;
        weread_result_t *result = test_otp_snapshot(WR_VERIFY_REQUIRED, true);
        weread_result_free(result);
        ++otp.phase;
        if (otp.mode == OTP_IDLE) test_otp_invalidate();
        else if (otp.mode == OTP_ALLOCATING)
        {
            sim.allocation_hook = test_otp_invalidate;
            assert(!weread_submit_code(otp.login, "0012"));
        }
        else
        {
            otp.submitted = test_otp_submit(otp.login, "0012");
            if (otp.mode == OTP_QUEUED) test_otp_invalidate();
            else sim.client_hook = test_otp_invalidate_client;
        }
        return;
    }
    assert(otp.invalidated && !sim.store_writes && !sim.session_exports && !sim.shelf_calls);
    assert(sim.poll_calls == 1 && !weread_submit_code(otp.login, "0012"));
    if (otp.event == OTP_NEW_QR)
    {
        assert(sim.login_calls == 2 && sim.wait_ticks == 2000);
        assert(published->generation == otp.replacement && published->result == WR_PENDING && !published->verification);
        assert(!weread_submit_code(otp.replacement, "0012"));
    }
    else assert(published->generation == otp.login && sim.wait_ticks == RT_WAITING_FOREVER);
    sim.service.cancelled = true;
}

static void test_otp_submit_oom_wait(unsigned step)
{
    (void)step;
    if (test_otp_qr_wait()) return;
    if (!otp.phase)
    {
        weread_result_t *result = test_otp_snapshot(WR_VERIFY_REQUIRED, true);
        unsigned live = sim.live, wakes = sim.wakes;
        sim.fail_allocation = sim.allocations + otp.fail_offset;
        assert(!weread_submit_code(otp.login, "0012"));
        assert(!pending && generation == otp.login && sim.live == live && sim.wakes == wakes);
        assert(published == result && result->verification && !weread_failed(otp.login));
        sim.fail_allocation = 0;
        otp.submitted = test_otp_submit(otp.login, "0012");
        weread_result_free(result);
    }
    else { assert(otp.phase == 1 && sim.verify_calls == 1); test_otp_success(); }
    ++otp.phase;
}

static void test_otp_result_oom_wait(unsigned step)
{
    (void)step;
    if (!otp.phase)
    {
        if (test_otp_qr_wait()) return;
        weread_result_t *result = test_otp_snapshot(WR_VERIFY_REQUIRED, true);
        sim.verify_result = WR_VERIFY_FAILED;
        otp.submitted = test_otp_submit(result->generation, "0012");
        weread_result_free(result);
        sim.fail_allocation = sim.allocations + otp.fail_offset;
    }
    else if (otp.phase == 1)
    {
        assert(sim.wait_ticks == RT_WAITING_FOREVER && sim.verify_calls == 1 && sim.poll_calls == 1);
        assert(weread_failed(otp.submitted) && !pending);
        assert(published->generation == otp.login && published->verification);
        assert(!sim.store_writes && !sim.session_exports && !sim.shelf_calls);
        /* No current-generation snapshot exists after result OOM. The failure
         * marker allows a new QR request, but neither token can retry OTP. */
        assert(!weread_submit_code(otp.login, "0012"));
        assert(!weread_submit_code(otp.submitted, "0012"));
        sim.fail_allocation = 0;
        otp.replacement = weread_submit(WEREAD_LOGIN, NULL, NULL, NULL, NULL);
        assert(otp.replacement && !weread_failed(otp.submitted));
    }
    else
    {
        assert(otp.phase == 2 && sim.login_calls == 2 && sim.poll_calls == 1);
        assert(sim.wait_ticks == 2000 && published->generation == otp.replacement);
        assert(published->result == WR_PENDING && !published->verification);
        sim.service.cancelled = true;
    }
    ++otp.phase;
}

static void test_otp(void)
{
    unsigned before = test_cases;
    test_otp_start(test_otp_idle_wait); test_run();
    assert(otp.phase == 3 && sim.poll_calls == 1 && !sim.verify_calls); test_cleanup();

    test_otp_start(test_otp_wrong_wait); sim.client_hook = test_otp_saved_before_shelf;
    test_run(); assert(otp.phase == 3 && sim.log_calls == 3); test_cleanup();

    const wr_result_t retriable[] = {WR_VERIFY_FAILED, WR_VERIFY_REQUIRED, WR_ERR_HTTP, WR_ERR_MEMORY};
    for (unsigned i = 0; i < sizeof(retriable) / sizeof(retriable[0]); ++i)
    {
        test_otp_start(test_otp_retry_wait); otp.outcome = retriable[i];
        sim.client_hook = test_otp_saved_before_shelf;
        test_run(); test_cleanup();
    }
    for (unsigned i = 0; i < 5; ++i)
    {
        test_otp_start(test_otp_retry_wait); otp.outcome = i % 2 ? WR_ERR_MEMORY : WR_ERR_HTTP;
        otp.offline = i == 0; otp.init_retry = i == 1 || i == 2;
        if (i >= 3) { sim.poll_result = otp.outcome; sim.poll_waiting_otp = true; }
        sim.client_hook = test_otp_saved_before_shelf;
        test_run(); test_cleanup();
    }
    for (unsigned i = 0; i < 2; ++i)
    {
        test_otp_start(test_otp_terminal_wait); otp.outcome = i ? WR_VERIFY_LIMIT : WR_VERIFY_EXPIRED;
        test_run(); test_cleanup();
    }
    for (unsigned mode = 0; mode < OTP_MODES; ++mode)
    {
        for (unsigned event = 0; event < OTP_EVENTS; ++event)
        {
            test_otp_start(test_otp_invalidation_wait); otp.mode = mode; otp.event = event;
            test_run();
            assert(otp.invalidated && !sim.store_writes && !sim.session_exports && !sim.shelf_calls);
            assert(sim.verify_calls == (unsigned)(mode >= OTP_IN_FLIGHT ||
                (mode == OTP_QUEUED && event == OTP_CANCEL)));
            assert(published && published->generation == (event == OTP_NEW_QR ? otp.replacement : otp.login));
            test_cleanup();
        }
    }
    for (unsigned i = 1; i <= 5; ++i)
    {
        test_otp_start(test_otp_submit_oom_wait); otp.fail_offset = i;
        sim.client_hook = test_otp_saved_before_shelf;
        test_run(); test_cleanup();
    }
    for (unsigned i = 1; i <= 7; ++i)
    {
        test_otp_start(test_otp_result_oom_wait); otp.fail_offset = i;
        test_run(); test_cleanup();
    }
    printf("OTP: %u scenarios passed (idle polling, ASCII codes, retry, terminal outcomes, invalidation windows and allocation failures)\n",
           test_cases - before);
    puts("OTP result OOM: all 7 allocation failures mark the request failed; current behavior requires a new QR, not OTP resubmission");
}

static void test_faults(void)
{
    test_reset(); test_start();
    weread_submit(WEREAD_SHELF, NULL, NULL, NULL, NULL); test_run();
    unsigned allocations = sim.allocations;
    test_cleanup();
    for (unsigned i = 1; i <= allocations; ++i)
    {
        test_reset(); test_start(); sim.fail_allocation = i;
        weread_submit(WEREAD_SHELF, NULL, NULL, NULL, NULL); test_run(); test_cleanup();
    }
    test_reset(); test_start();
    uint32_t serial = weread_submit(WEREAD_SHELF, NULL, NULL, NULL, NULL);
    sim.fail_allocation = sim.allocations + 2; /* Client allocation, then result. */
    test_run(); assert(weread_failed(serial) && !published); test_cleanup();

    test_reset(); sim.client_failure = true; test_start();
    weread_submit(WEREAD_SHELF, NULL, NULL, NULL, NULL); test_run();
    weread_result_t *result = test_snapshot(WEREAD_SHELF, WR_ERR_MEMORY);
    assert(!sim.shelf_calls); weread_result_free(result); test_cleanup();
    for (unsigned i = 1; i <= 4; ++i)
    {
        test_reset(); test_start(); sim.fail_allocation = i;
        weread_result_t content = test_content(); reader_position_t position = test_position(4);
        assert(!weread_save_position(&content, &position) && !save_pending && !sim.live);
        test_run(); test_cleanup();
    }
    test_reset(); test_start(); sim.store_failure = true;
    weread_submit(WEREAD_CHAPTER, "book", "title", "c2", "chapter"); test_run();
    result = test_snapshot(WEREAD_CHAPTER, WR_ERR_REMOTE);
    assert(result->error[0] && sim.chapter_calls == 1); weread_result_free(result); test_cleanup();
    printf("faults: each of %u successful-path allocations, result OOM, position OOM, client/store failures passed\n", allocations);
}

int main(void)
{
    test_admission();
    test_generations();
    test_snapshot_lifetime();
    test_accounts_cache();
    test_restore_position();
    test_content_revisions();
    test_exit_drain();
    test_cover_worker_lifecycle();
    test_login_logout();
    test_otp();
    test_faults();
    test_reset();
    printf("PASS: %u real-service lifecycle scenarios; no outstanding allocations, clients or queued work\n", test_cases);
    return 0;
}
