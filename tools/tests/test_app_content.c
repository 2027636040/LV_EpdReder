#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <sys/stat.h>
#include "cJSON.h"
#include "constants.h"
#include "intent.h"
#include "intent_int.h"

typedef int rt_err_t;
typedef enum { STORAGE_FLASH, STORAGE_SD, STORAGE_COUNT } storage_volume_t;
enum { STORAGE_APP_CODE, RT_NAME_MAX = 8, GUI_APP_MSG_RUN_APP = 10 };
#define STORAGE_FLASH_APPS "/flash/apps"
#define APP_RECOVERY_BLOCKED (-EACCES)
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#include "content_headers.h"

static epd_app_entry_t installed[24];
static const char *manifests[24];
static unsigned installed_count, queued_count;
static bool available = true, changing, blocked, directory, install_busy;
static uint32_t card_session = 10;
static int locks, allocation_count, fail_allocation = -1, queue_result;
static _intent queued;
static const int ui_icon_album = 0;

void *content_test_malloc(size_t size)
{
    if (fail_allocation == 0) return NULL;
    if (fail_allocation > 0) --fail_allocation;
    void *memory = malloc(size);
    if (memory) ++allocation_count;
    return memory;
}
void content_test_free(void *memory)
{
    if (memory) --allocation_count;
    free(memory);
}
static void storage_lock(void) { ++locks; }
static void storage_unlock(void) { assert(locks > 0); --locks; }
static bool storage_changing(void) { return changing; }
static bool storage_path_available(const char *path) { (void)path; return available; }
static const char *storage_path_root(const char *path)
{ return !strncmp(path, "/sdcard/", 8) ? STORAGE_SD_ROOT : STORAGE_FLASH_ROOT; }
static uint32_t storage_card_session(void) { return card_session; }
static rt_err_t app_recovery_check_dynamic(void) { return blocked ? APP_RECOVERY_BLOCKED : RT_EOK; }
static bool app_installer_blocks(const char *id) { (void)id; return install_busy; }
static bool app_installer_volume_ready(storage_volume_t v) { (void)v; return available; }
static bool storage_apps_enabled(storage_volume_t v) { (void)v; return available; }
unsigned app_catalog_count(void) { return installed_count; }
const epd_app_entry_t *app_catalog_get(unsigned i) { return i < installed_count ? &installed[i] : NULL; }
static bool storage_app_locate(const char *id, storage_volume_t *v)
{
    for (unsigned i = 0; i < installed_count; ++i)
        if (!strcmp(id, installed[i].id)) { *v = installed[i].volume; return available; }
    return false;
}
static bool storage_app_path(char *out, size_t size, const char *id, int area, const char *relative)
{
    (void)area; (void)relative;
    int n = snprintf(out, size, "/flash/apps/%s", id);
    return n > 0 && (size_t)n < size;
}
static int content_stat(const char *path, struct stat *st)
{
    (void)path;
    memset(st, 0, sizeof(*st));
    st->st_mode = (unsigned short)(directory ? _S_IFDIR : _S_IFREG);
    st->st_size = 100;
    return available ? 0 : -1;
}
#define stat(path, st) content_stat(path, st)
static cJSON *manifest_read(const char *root)
{
    const char *id = strrchr(root, '/');
    for (unsigned i = 0; id && i < installed_count; ++i)
        if (!strcmp(id + 1, installed[i].id)) return cJSON_Parse(manifests[i]);
    return NULL;
}
#include "package_under_test.h"
#include "content_under_test.h"

typedef struct
{
    int msg_id;
    void *handler;
    union { _intent intnt; } content;
} gui_app_msg_t;
static int send_msg_to_gui_app_task(gui_app_msg_t *msg)
{
    assert(msg->msg_id == GUI_APP_MSG_RUN_APP && !msg->handler);
    if (queue_result) return queue_result;
    queued = msg->content.intnt;
    ++queued_count;
    return RT_EOK;
}
#include "sdk_run_under_test.h"

static void add_handler(const char *id, const char *manifest)
{
    unsigned i = installed_count++;
    snprintf(installed[i].id, sizeof(installed[i].id), "%s", id);
    manifests[i] = manifest;
    char root[96];
    snprintf(root, sizeof(root), "/flash/apps/%s", id);
    assert(app_package_metadata(root, id, &installed[i]));
}

static void manifest_cases(void)
{
    const char *bad[] = {"null", "true", "3", "\"txt\"", "{}", "[1]", "[\"\"]",
                         "[\".txt\"]", "[\"TXT\"]", "[\"txt\",\"txt\"]",
                         "[\"t xt\"]", "[\"a/b\"]", "[\"中文\"]", "[[]]"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
    {
        char text[256];
        snprintf(text, sizeof(text), "{\"file_extensions\":%s}", bad[i]);
        cJSON *json = cJSON_Parse(text);
        assert(json && !content_extensions_valid(json, false));
        cJSON_Delete(json);
    }
    cJSON *json = cJSON_Parse("{\"file_extensions\":[\"txt\",\"7z\"]}");
    assert(content_extensions_valid(json, false));
    assert(!content_extensions_valid(json, true));
    cJSON_Delete(json);
    json = cJSON_CreateObject();
    assert(content_extensions_valid(json, true));
    cJSON *extensions = cJSON_AddArrayToObject(json, "file_extensions");
    assert(content_extensions_valid(json, true));
    for (unsigned i = 0; i < 600; ++i)
    {
        char ext[400];
        memset(ext, 'a', 300);
        snprintf(ext + 300, sizeof(ext) - 300, "%u", i);
        cJSON_AddItemToArray(extensions, cJSON_CreateString(ext));
    }
    assert(content_extensions_valid(json, false));
    cJSON_Delete(json);
}

int main(void)
{
    manifest_cases();
    add_handler("books", "{\"name\":\"书架\",\"file_extensions\":[\"txt\",\"epub\"]}");
    add_handler("gallery", "{\"name\":\"图库\",\"file_extensions\":[\"jpg\",\"png\"]}");
    const char *path = "/sdcard/中文 目录/完整 书名.TXT";
    app_content_handler_t matches[2];
    unsigned count;
    assert(app_content_query(path, 0, matches, 2, &count) == RT_EOK && count == 1);
    assert(!strcmp(matches[0].id, "books"));
    assert(app_content_query("/flash/a.unknown", 0, matches, 2, &count) == RT_EOK && count == 0);
    assert(!app_package_handles_path("/flash/apps/books", "/flash/a.txt/no-extension"));
    assert(!app_package_handles_path("/flash/apps/books", "/flash/.txt"));
    assert(!app_package_handles_path("/flash/apps/books", "/flash/a.txt."));
    add_handler("other", "{\"name\":\"Other\",\"file_extensions\":[\"txt\"]}");
    assert(app_content_query(path, 1, matches, 1, &count) == RT_EOK && count == 2);
    assert(!strcmp(matches[0].id, "other"));
    memset(installed[2].name, 'x', sizeof(installed[2].name) - 1);
    assert(!strcmp(matches[0].name, "Other"));
    assert(app_content_query(path, UINT_MAX, NULL, 0, &count) == RT_EOK && count == 2);
    assert(queued_count == 0);

    assert(app_content_open("books", path) == RT_EOK);
    assert(!strcmp(intent_get_action(&queued), "books"));
    assert(!strcmp(intent_get_string(&queued, "path"), path));
    char received[STORAGE_PATH_MAX];
    assert(app_content_receive(&queued, received, sizeof(received)) == RT_EOK);
    assert(!strcmp(received, path));
    _intent prior = queued;
    assert(app_content_open("books", path) == RT_EOK);
    assert(memcmp(&prior, &queued, sizeof(queued)) != 0);
    assert(app_content_receive(&queued, received, 3) == -ENAMETOOLONG && !received[0]);
    ++card_session;
    assert(app_content_receive(&queued, received, sizeof(received)) == -RT_EIO);
    assert(!received[0]);
    intent_t ordinary = intent_init("books");
    assert(app_content_receive(ordinary, received, sizeof(received)) == -ENOENT);
    intent_deinit(ordinary);
    blocked = true;
    unsigned before = queued_count;
    assert(app_content_open("books", path) == APP_RECOVERY_BLOCKED);
    blocked = false;
    available = false;
    assert(app_content_open("books", path) == -RT_EIO);
    assert(app_content_receive(&queued, received, sizeof(received)) == -RT_EIO);
    available = true;
    changing = true;
    assert(app_content_open("books", path) == -RT_EIO);
    changing = false;
    install_busy = true;
    assert(app_content_open("books", path) == -ENOENT);
    install_busy = false;
    directory = true;
    assert(app_content_open("books", path) == -RT_EIO);
    directory = false;
    assert(app_content_open("gone", path) == -ENOENT);
    assert(app_content_open("gallery", path) == -ENOENT);
    assert(app_content_open("books", "/sdcard/a/../b.txt") == -RT_EINVAL);
    assert(app_content_open("books", "/sdcardish/a.txt") == -RT_EINVAL);
    assert(queued_count == before);
    queue_result = -RT_EFULL;
    assert(app_content_open("books", path) == -RT_EFULL);
    queue_result = 0;
    for (int allocation = 0; allocation < 5; ++allocation)
    {
        fail_allocation = allocation;
        assert(app_content_open("books", path) == -RT_ENOMEM);
        assert(allocation_count == 0 && locks == 0);
    }
    fail_allocation = -1;

    /* Measure the boundary through SDK calls, with maximum session/serial text.
     * No duplicated guessed 128-byte formula determines the expected result. */
    card_session = UINT32_MAX;
    unsigned accepted = 0, rejected = 0;
    for (unsigned utf8 = 0; utf8 < 2; ++utf8)
    for (unsigned length = 13; length < 200; ++length)
    {
        char candidate[256];
        memset(candidate, 'x', length);
        memcpy(candidate, "/sdcard/", 8);
        const char chinese[] = "中文 书";
        if (utf8 && length >= 8 + sizeof(chinese) + 4)
            memcpy(candidate + 8, chinese, sizeof(chinese) - 1);
        memcpy(candidate + length - 4, ".txt", 4);
        candidate[length] = 0;
        intent_t reference = intent_init("books");
        int a = intent_set_string(reference, "path", candidate);
        int b = a == RT_EOK ? intent_set_string(reference, "s", "ffffffff") : RT_ERROR;
        int c = b == RT_EOK ? intent_set_string(reference, "r", "ffffffff") : RT_ERROR;
        bool fits = a == RT_EOK && b == RT_EOK && c == RT_EOK;
        intent_deinit(reference);
        request_serial = UINT32_MAX - 1;
        rt_err_t result = app_content_open("books", candidate);
        assert(result == (fits ? RT_EOK : -ENAMETOOLONG));
        if (fits)
        {
            ++accepted;
            assert(app_content_receive(&queued, received, sizeof(received)) == RT_EOK);
            assert(!strcmp(received, candidate));
        }
        else ++rejected;
    }
    assert(accepted && rejected && !allocation_count && !locks);
    printf("content: manifest/discovery, copied selection, real SDK intent, repeated open, card session, policy, failures and capacity passed (%u fit, %u rejected)\n", accepted, rejected);
    return 0;
}
