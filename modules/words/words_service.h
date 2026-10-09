#ifndef WORDS_SERVICE_H
#define WORDS_SERVICE_H

#include "platform/epd_app.h"
#include "words_dictionary.h"
#include "words_learning.h"

typedef enum { WORDS_SEARCH, WORDS_DETAIL, WORDS_HOME, WORDS_NEXT, WORDS_RATE,
               WORDS_COLLECTION, WORDS_COLLECT, WORDS_PAUSE, WORDS_QUOTA,
               WORDS_BOOKS, WORDS_BOOK_SELECT } words_job_kind_t;
typedef struct
{
    uint32_t serial, entry, slot, token, value;
    words_job_kind_t kind;
    char query[WORDS_KEY_SIZE], path[STORAGE_PATH_MAX];
    unsigned char identity[24];
} words_job_t;

typedef struct
{
    uint32_t serial;
    uint32_t slot, token, flags, quota, offset;
    words_counts_t counts;
    bool clock_valid, accepted;
    char book[WORDS_KEY_SIZE];
    char book_paths[WORDS_MATCH_MAX][STORAGE_PATH_MAX];
    words_matches_t matches;
    char names[WORDS_MATCH_MAX][WORDS_KEY_SIZE];
    char path[STORAGE_PATH_MAX], error[96];
    unsigned char identity[24];
    char content[WORDS_ENTRY_MAX];
    const char *fields[WORDS_FIELD_COUNT];
} words_result_t;

bool words_service_start(void);
bool words_service_flush(void);
void words_service_stop(void);
uint32_t words_submit(const words_job_t *job);
void words_cancel(uint32_t serial);
words_result_t *words_take(uint32_t serial, bool *failed);

#endif
