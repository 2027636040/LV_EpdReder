#ifndef WEREAD_SERVICE_H
#define WEREAD_SERVICE_H
#include "weread_store.h"
#include "client/wr_client.h"
#include "../common/reader/reader.h"

typedef enum { WEREAD_RESTORE, WEREAD_LOGIN, WEREAD_SHELF, WEREAD_CATALOG,
               WEREAD_CHAPTER, WEREAD_LOGOUT, WEREAD_CATALOG_RESTORE, WEREAD_VERIFY } weread_action_t;
typedef struct weread_result
{
    unsigned references;
    uint32_t generation, revision;
    weread_action_t action;
    wr_result_t result;
    bool offline, html, verification;
    char error[128];
    char *account, *name, *qr, *book, *title, *chapter, *chapter_title;
    wr_book_t *books;
    size_t book_count;
    wr_chapter_t *chapters;
    size_t chapter_count;
    char path[STORAGE_PATH_MAX];
    uint32_t signature;
    reader_position_t restore;
    char *last_chapter;
} weread_result_t;

bool weread_start(void);
bool weread_stopping(void);
void weread_stop(void);
uint32_t weread_submit(weread_action_t action, const char *book, const char *title,
                        const char *chapter, const char *chapter_title);
uint32_t weread_submit_chapter(const weread_result_t *catalog, size_t index);
/* Continue only the still-current login challenge. The service copies the code. */
uint32_t weread_submit_code(uint32_t login_generation, const char *code);
void weread_cancel(uint32_t generation);
bool weread_cancelled(uint32_t generation);
bool weread_unavailable(void);
bool weread_failed(uint32_t generation);
weread_result_t *weread_result_get(void);
void weread_result_retain(weread_result_t *result);
void weread_result_free(weread_result_t *result);
bool weread_save_position(const weread_result_t *content, const reader_position_t *position);
void weread_wake(void);
/* Called only by the platform's UI service dispatcher. */
bool weread_ui_process(epd_service_t *service, bool stopping);
char *weread_string(const char *text);
#endif
