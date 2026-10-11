#include "weread_service.h"
#include "weread_covers.h"
#include <rthw.h>
#include <dlmodule.h>
#include <string.h>
#include <stdio.h>

typedef struct
{
    uint32_t generation;
    int64_t content_revision;
    weread_action_t action;
    char *book, *title, *chapter, *chapter_title;
    char otp[5];
} request_t;
typedef struct
{
    uint32_t version, signature, uid_size;
    reader_position_t position;
} position_record_t;
typedef struct
{
    char *account, *book, *chapter;
    uint32_t signature;
    reader_position_t position;
} pending_position_t;

static epd_service_t *worker;
static request_t *pending;
static weread_result_t *published;
static pending_position_t *save_pending;
static uint32_t generation, revision, failed_generation;
static bool stopped;

char *weread_string(const char *text)
{
    if (!text) text = "";
    size_t size = strlen(text) + 1;
    char *copy = epd_app_alloc(size, EPD_APP_PSRAM);
    if (copy) memcpy(copy, text, size);
    return copy;
}

void weread_result_retain(weread_result_t *result)
{
    if (!result) return;
    rt_base_t level = rt_hw_interrupt_disable();
    ++result->references;
    rt_hw_interrupt_enable(level);
}

void weread_result_free(weread_result_t *result)
{
    if (!result) return;
    rt_base_t level = rt_hw_interrupt_disable();
    bool release = --result->references == 0;
    rt_hw_interrupt_enable(level);
    if (!release) return;
    wr_books_free(result->books, result->book_count);
    wr_chapters_free(result->chapters, result->chapter_count);
    epd_app_free(result->account); epd_app_free(result->name); epd_app_free(result->qr);
    epd_app_free(result->book); epd_app_free(result->title); epd_app_free(result->chapter);
    epd_app_free(result->chapter_title); epd_app_free(result->last_chapter);
    epd_app_free(result);
}

weread_result_t *weread_result_get(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    weread_result_t *result = published;
    if (result) ++result->references;
    rt_hw_interrupt_enable(level);
    return result;
}

bool weread_cancelled(uint32_t serial)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool cancel = stopped || serial != generation || (worker && epd_service_cancelled(worker));
    rt_hw_interrupt_enable(level);
    return cancel || !storage_app_available("weread");
}

bool weread_unavailable(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool cancel = !worker || stopped || epd_service_cancelled(worker);
    rt_hw_interrupt_enable(level);
    return cancel || !storage_app_available("weread");
}

bool weread_failed(uint32_t serial)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool failed = serial && serial == failed_generation;
    rt_hw_interrupt_enable(level);
    return failed;
}

static bool cancel_request(void *user)
{
    return weread_cancelled(((request_t *)user)->generation);
}

void weread_wake(void)
{
    if (worker) epd_service_wake(worker);
}

static void request_free(request_t *request)
{
    if (!request) return;
    epd_app_free(request->book); epd_app_free(request->title);
    epd_app_free(request->chapter); epd_app_free(request->chapter_title);
    epd_app_free(request);
}

static uint32_t submit(weread_action_t action, const char *book, const char *title,
                        const char *chapter, const char *chapter_title, int64_t content_revision,
                        uint32_t login_generation, const char *otp)
{
    if (!worker || stopped || epd_service_cancelled(worker)) return 0;
    request_t *request = epd_app_alloc(sizeof(*request), EPD_APP_PSRAM);
    if (!request) return 0;
    memset(request, 0, sizeof(*request));
    request->action = action;
    request->content_revision = content_revision;
    if (otp) memcpy(request->otp, otp, sizeof(request->otp));
    request->book = weread_string(book); request->title = weread_string(title);
    request->chapter = weread_string(chapter); request->chapter_title = weread_string(chapter_title);
    if (!request->book || !request->title || !request->chapter || !request->chapter_title)
    { request_free(request); return 0; }
    rt_base_t level = rt_hw_interrupt_disable();
    if (!worker || stopped || epd_service_cancelled(worker) ||
        (action == WEREAD_VERIFY && (!login_generation || login_generation != generation ||
         !published || published->generation != login_generation || !published->verification)))
    { rt_hw_interrupt_enable(level); request_free(request); return 0; }
    request_t *old = pending;
    request->generation = ++generation;
    if (!request->generation) request->generation = ++generation;
    pending = request;
    failed_generation = 0;
    uint32_t serial = request->generation;
    rt_hw_interrupt_enable(level);
    request_free(old);
    weread_wake();
    return serial;
}

uint32_t weread_submit(weread_action_t action, const char *book, const char *title,
                        const char *chapter, const char *chapter_title)
{
    if (action == WEREAD_VERIFY) return 0;
    return submit(action, book, title, chapter, chapter_title, 0, 0, NULL);
}

uint32_t weread_submit_code(uint32_t login_generation, const char *code)
{
    if (!code || strlen(code) != 4) return 0;
    for (unsigned i = 0; i < 4; ++i) if (code[i] < '0' || code[i] > '9') return 0;
    return submit(WEREAD_VERIFY, NULL, NULL, NULL, NULL, 0, login_generation, code);
}

uint32_t weread_submit_chapter(const weread_result_t *catalog, size_t index)
{
    if (!catalog || index >= catalog->chapter_count) return 0;
    const wr_chapter_t *chapter = &catalog->chapters[index];
    return submit(WEREAD_CHAPTER, catalog->book, catalog->title, chapter->uid,
                  chapter->title, chapter->update_time, 0, NULL);
}

void weread_cancel(uint32_t serial)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (serial == generation) ++generation;
    rt_hw_interrupt_enable(level);
    weread_wake();
}

static void publish(weread_result_t *result)
{
    rt_base_t level = rt_hw_interrupt_disable();
    weread_result_t *old = NULL;
    if (!stopped && worker && !epd_service_cancelled(worker) && result->generation == generation)
    {
        result->revision = ++revision;
        old = published;
        published = result;
        ++result->references;
    }
    rt_hw_interrupt_enable(level);
    weread_result_free(old);
}

static const char *error_text(wr_result_t result)
{
    switch (result)
    {
    case WR_ERR_MEMORY: return "内存不足，请稍后重试";
    case WR_ERR_AUTH: return "登录已失效，请重新扫码";
    case WR_EXPIRED: return "二维码已过期，请重新登录";
    case WR_VERIFY_REQUIRED: return "请输入手机上显示的四位验证码";
    case WR_VERIFY_FAILED: return "验证码错误，请重新输入";
    case WR_VERIFY_EXPIRED: return "验证码已过期，请重新扫码";
    case WR_VERIFY_LIMIT: return "验证次数已达上限，请重新扫码";
    case WR_ERR_PERMISSION: return "当前账号暂时无法阅读本章";
    case WR_ERR_INTEGRITY: return "章节校验失败，请重试";
    case WR_ERR_CANCELLED: return "操作已取消";
    case WR_ERR_HTTP: return "网络请求失败，请检查连接后重试";
    default: return "微信读书返回异常，请稍后重试";
    }
}

static weread_result_t *result_new(const request_t *request, wr_client_t *client)
{
    weread_result_t *result = epd_app_alloc(sizeof(*result), EPD_APP_PSRAM);
    if (!result) return NULL;
    memset(result, 0, sizeof(*result));
    result->references = 1; result->generation = request->generation; result->action = request->action;
    result->account = weread_string(client ? wr_client_account_id(client) : NULL);
    result->name = weread_string(client ? wr_client_account_name(client) : NULL);
    result->book = weread_string(request->book); result->title = weread_string(request->title);
    result->chapter = weread_string(request->chapter); result->chapter_title = weread_string(request->chapter_title);
    if (!result->account || !result->name || !result->book || !result->title ||
        !result->chapter || !result->chapter_title)
    { weread_result_free(result); return NULL; }
    return result;
}

static void save_session(wr_client_t *client)
{
    char *json = NULL;
    size_t size = 0;
    if (wr_client_session_export(client, &json, &size) == WR_OK)
        weread_store_save(STORAGE_APP_DATA, NULL, NULL, "session", json, size);
    epd_app_free(json);
}

static void position_free(pending_position_t *position)
{
    if (!position) return;
    epd_app_free(position->account); epd_app_free(position->book);
    epd_app_free(position->chapter); epd_app_free(position);
}

bool weread_save_position(const weread_result_t *content, const reader_position_t *position)
{
    if (!content || !worker || stopped || epd_service_cancelled(worker)) return false;
    pending_position_t *saved = epd_app_alloc(sizeof(*saved), EPD_APP_PSRAM);
    if (!saved) return false;
    memset(saved, 0, sizeof(*saved));
    saved->account = weread_string(content->account); saved->book = weread_string(content->book);
    saved->chapter = weread_string(content->chapter); saved->signature = content->signature;
    saved->position = *position;
    if (!saved->account || !saved->book || !saved->chapter) { position_free(saved); return false; }
    rt_base_t level = rt_hw_interrupt_disable();
    if (stopped || epd_service_cancelled(worker))
    { rt_hw_interrupt_enable(level); position_free(saved); return false; }
    pending_position_t *old = save_pending;
    save_pending = saved;
    rt_hw_interrupt_enable(level);
    position_free(old);
    weread_wake();
    return true;
}

static void position_flush(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    pending_position_t *saved = save_pending;
    save_pending = NULL;
    rt_hw_interrupt_enable(level);
    if (!saved) return;
    size_t uid_size = strlen(saved->chapter) + 1;
    if (uid_size <= UINT32_MAX && uid_size <= SIZE_MAX - sizeof(position_record_t))
    {
        position_record_t *record = epd_app_alloc(sizeof(*record) + uid_size, EPD_APP_PSRAM);
        if (record)
        {
            memset(record, 0, sizeof(*record));
            record->version = 1; record->signature = saved->signature;
            record->uid_size = uid_size; record->position = saved->position;
            memcpy(record + 1, saved->chapter, uid_size);
            weread_store_save(STORAGE_APP_DATA, saved->account, saved->book, "position", record,
                              sizeof(*record) + uid_size);
            epd_app_free(record);
        }
    }
    position_free(saved);
}

static void position_load(weread_result_t *result)
{
    size_t size;
    position_record_t *record = weread_store_load(STORAGE_APP_DATA, result->account, result->book, "position", &size);
    if (!record) return;
    if (size > sizeof(*record) && record->version == 1 && record->uid_size == size - sizeof(*record) &&
        ((char *)(record + 1))[record->uid_size - 1] == 0 &&
        strlen((char *)(record + 1)) + 1 == record->uid_size)
    {
        result->last_chapter = weread_string((char *)(record + 1));
        if (result->chapter[0] && !strcmp(result->chapter, (char *)(record + 1)) &&
            result->signature == record->signature) result->restore = record->position;
    }
    epd_app_free(record);
}

static bool cache_list(weread_result_t *result)
{
    size_t size;
    bool shelf = result->action == WEREAD_SHELF || result->action == WEREAD_RESTORE;
    char *json = weread_store_load(STORAGE_APP_CACHE, result->account, shelf ? NULL : result->book,
                                   shelf ? "shelf" : "catalog", &size);
    if (!json) return false;
    wr_result_t status = shelf ? wr_books_parse(json, size, &result->books, &result->book_count) :
        wr_chapters_parse(json, size, &result->chapters, &result->chapter_count);
    epd_app_free(json);
    if (status == WR_OK) { result->offline = true; result->result = WR_OK; }
    return status == WR_OK;
}

static void fetch(request_t *request, wr_client_t *client, weread_result_t *result)
{
    char *json = NULL;
    size_t size = 0;
    bool online = network_ready();
    if (request->action == WEREAD_RESTORE || request->action == WEREAD_SHELF)
    {
        if (!online || request->action == WEREAD_RESTORE)
            if (cache_list(result)) return;
        result->result = online ? wr_client_shelf(client, &result->books, &result->book_count) : WR_ERR_HTTP;
        if (result->result == WR_OK && !cancel_request(request) && wr_books_json(result->books, result->book_count, &json, &size) == WR_OK)
            weread_store_save(STORAGE_APP_CACHE, result->account, NULL, "shelf", json, size);
        else if (result->result != WR_OK) cache_list(result);
    }
    else if (request->action == WEREAD_CATALOG || request->action == WEREAD_CATALOG_RESTORE)
    {
        if (request->action == WEREAD_CATALOG_RESTORE && cache_list(result))
        { position_load(result); return; }
        result->result = online ? wr_client_catalog(client, request->book, &result->chapters, &result->chapter_count) : WR_ERR_HTTP;
        if (result->result == WR_OK && !cancel_request(request) && wr_chapters_json(result->chapters, result->chapter_count, &json, &size) == WR_OK)
            weread_store_save(STORAGE_APP_CACHE, result->account, request->book, "catalog", json, size);
        else if (result->result != WR_OK) cache_list(result);
        position_load(result);
    }
    else if (request->action == WEREAD_CHAPTER)
    {
        if (weread_store_cached_chapter(result->account, result->book, result->chapter,
                                        request->content_revision, result->path, &result->signature))
        { result->result = WR_OK; result->offline = !online; }
        else
        {
            result->result = online ? wr_client_chapter(client, result->book, result->chapter, &json, &size) : WR_ERR_HTTP;
            if (result->result == WR_OK && !cancel_request(request))
            {
                const char *start = json;
                while (*start == ' ' || *start == '\r' || *start == '\n' || *start == '\t') ++start;
                bool html = *start == '<';
                if (!weread_store_chapter(result->account, result->book, result->chapter,
                                           request->content_revision, html, json, size,
                                           result->path, &result->signature))
                {
                    result->result = WR_ERR_REMOTE;
                    snprintf(result->error, sizeof(result->error), "章节缓存写入失败，请检查存储空间");
                }
            }
        }
        const char *extension = strrchr(result->path, '.');
        result->html = extension && !strcmp(extension, ".xhtml");
        if (result->result == WR_OK) position_load(result);
    }
    epd_app_free(json);
    if (online && !cancel_request(request)) save_session(client);
}

static void login_result(request_t *request, wr_client_t *client, wr_result_t status)
{
    if (cancel_request(request)) return;
    request->action = WEREAD_LOGIN;
    weread_result_t *result = result_new(request, client);
    if (result)
    {
        if (cancel_request(request)) { weread_result_free(result); return; }
        result->result = status;
        result->verification = wr_client_login_waiting_otp(client);
        if (status == WR_OK)
        {
            save_session(client);
            request->action = result->action = WEREAD_SHELF;
            fetch(request, client, result);
        }
        else snprintf(result->error, sizeof(result->error), "%s", status == WR_ERR_PERMISSION ?
            "登录请求被拒绝，请稍后重试" : error_text(status));
        publish(result);
        weread_result_free(result);
    }
    else
    {
        rt_base_t level = rt_hw_interrupt_disable();
        failed_generation = request->generation;
        rt_hw_interrupt_enable(level);
    }
    /* Only the outcome is recorded; codes, cookies and tokens stay private. */
    rt_kprintf("[weread] login result=%d verify_wait=%d\n", (int)status,
               client && wr_client_login_waiting_otp(client));
}

static void run(epd_service_t *service)
{
    wr_client_t *client = wr_client_create(NULL, NULL);
    size_t size;
    char *session = weread_store_load(STORAGE_APP_DATA, NULL, NULL, "session", &size);
    if (client && session && size) wr_client_session_import(client, session, size);
    epd_app_free(session);
    request_t *login = NULL;
    while (!epd_service_cancelled(service))
    {
        position_flush();
        rt_base_t level = rt_hw_interrupt_disable();
        request_t *request = pending;
        pending = NULL;
        rt_hw_interrupt_enable(level);
        if (request)
        {
            request_free(login); login = NULL;
            if (client) wr_client_set_cancel(client, cancel_request, request);
            if (request->action == WEREAD_VERIFY)
            {
                wr_result_t status = !client ? WR_ERR_MEMORY : !network_ready() ? WR_ERR_HTTP :
                    wr_client_login_verify(client, request->otp);
                memset(request->otp, 0, sizeof(request->otp));
                login_result(request, client, status);
                if (client) wr_client_set_cancel(client, NULL, NULL);
                request_free(request);
                continue;
            }
            weread_result_t *result = result_new(request, client);
            if (result)
            {
                result->result = client ? WR_OK : WR_ERR_MEMORY;
                if (client && (request->action == WEREAD_LOGIN || request->action == WEREAD_LOGOUT ||
                    (request->action == WEREAD_RESTORE && !wr_client_authenticated(client))))
                {
                    if (request->action == WEREAD_LOGOUT)
                    {
                        if (!weread_store_logout())
                        {
                            result->result = WR_ERR_REMOTE;
                            snprintf(result->error, sizeof(result->error), "退出账号失败，请检查存储后重试");
                            publish(result); weread_result_free(result);
                            wr_client_set_cancel(client, NULL, NULL);
                            request_free(request);
                            continue;
                        }
                        wr_client_logout(client);
                    }
                    result->action = WEREAD_LOGIN;
                    result->result = network_ready() ? wr_client_login_begin(client, &result->qr) : WR_ERR_HTTP;
                    if (result->result == WR_OK) { login = request; request = NULL; result->result = WR_PENDING; }
                }
                else if (client) fetch(request, client, result);
                if (result->result < 0 && !result->error[0])
                    snprintf(result->error, sizeof(result->error), "%s", error_text(result->result));
                publish(result);
                weread_result_free(result);
            }
            else
            {
                level = rt_hw_interrupt_disable();
                failed_generation = request->generation;
                rt_hw_interrupt_enable(level);
            }
            if (!login && client) wr_client_set_cancel(client, NULL, NULL);
            request_free(request);
        }
        else if (login)
        {
            if (cancel_request(login)) { request_free(login); login = NULL; wr_client_set_cancel(client, NULL, NULL); }
            else
            {
                wr_result_t status = wr_client_login_poll(client);
                if (status != WR_PENDING)
                {
                    login_result(login, client, status);
                    request_free(login); login = NULL;
                    wr_client_set_cancel(client, NULL, NULL);
                }
            }
        }
        if (epd_reader_worker_step(service)) continue;
        if (weread_covers_step()) continue;
        epd_service_wait(service, login ? rt_tick_from_millisecond(2000) : RT_WAITING_FOREVER);
    }
    rt_base_t level = rt_hw_interrupt_disable();
    request_t *request = pending; pending = NULL;
    stopped = true;
    rt_hw_interrupt_enable(level);
    request_free(request);
    position_flush();
    epd_reader_worker_shutdown();
    weread_covers_shutdown();
    request_free(login);
    wr_client_destroy(client);
}

static bool ui_process(epd_service_t *service, bool stopping)
{
    if (!weread_ui_process(service, stopping)) return false;
    if (stopping)
    {
        rt_base_t level = rt_hw_interrupt_disable();
        weread_result_t *result = published; published = NULL;
        worker = NULL;
        rt_hw_interrupt_enable(level);
        weread_result_free(result);
    }
    return stopping;
}

static const epd_background_t definition = {.run = run, .stack_size = 12288, .priority = 25,
    .network = true, .ui_process = ui_process};

bool weread_start(void)
{
    if (worker) return !stopped;
    stopped = false;
    worker = app_service_start("weread", &definition, dlmodule_find("weread"));
    return worker != NULL;
}

bool weread_stopping(void) { return worker && stopped; }

void weread_stop(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    stopped = true; ++generation;
    rt_hw_interrupt_enable(level);
    app_service_stop("weread");
}
