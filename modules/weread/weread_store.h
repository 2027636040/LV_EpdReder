#ifndef WEREAD_STORE_H
#define WEREAD_STORE_H
#include "platform/epd_app.h"

/* The worker owns file I/O. Keys are server identities, never list positions. */
bool weread_store_path(char *path, storage_app_area_t area, const char *account,
                       const char *book, const char *name);
bool weread_store_save(storage_app_area_t area, const char *account, const char *book,
                       const char *name, const void *data, size_t size);
void *weread_store_load(storage_app_area_t area, const char *account, const char *book,
                        const char *name, size_t *size);
bool weread_store_chapter(const char *account, const char *book, const char *chapter,
                          int64_t content_revision, bool html, const void *data, size_t size,
                          char path[STORAGE_PATH_MAX], uint32_t *signature);
bool weread_store_cached_chapter(const char *account, const char *book, const char *chapter,
                                 int64_t content_revision,
                                 char path[STORAGE_PATH_MAX], uint32_t *signature);
bool weread_store_logout(void);
#endif
