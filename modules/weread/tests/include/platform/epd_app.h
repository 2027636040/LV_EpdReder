#ifndef TEST_EPD_APP_H
#define TEST_EPD_APP_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define STORAGE_PATH_MAX 512
#define EPD_APP_PSRAM 1
typedef enum { STORAGE_APP_CODE, STORAGE_APP_DATA, STORAGE_APP_CACHE } storage_app_area_t;
void *epd_app_alloc(size_t size, int region);
void epd_app_free(void *memory);
bool storage_app_available(const char *id);
bool storage_app_path(char *, size_t, const char *, storage_app_area_t, const char *);
bool storage_mkdirs(const char *);
uint32_t storage_crc32(const void *, size_t);
bool storage_file_replace(const char *, const void *, size_t);
bool storage_file_recover(const char *);
void storage_lock(void);
void storage_unlock(void);
#endif
