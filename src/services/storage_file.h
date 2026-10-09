#ifndef EPD_STORAGE_FILE_H
#define EPD_STORAGE_FILE_H

#include "storage.h"

typedef enum { STORAGE_APP_CODE, STORAGE_APP_DATA, STORAGE_APP_CACHE } storage_app_area_t;

#define STORAGE_FLASH_APPS STORAGE_FLASH_ROOT "/apps"
#define STORAGE_SD_APPS STORAGE_SD_ROOT "/.epd/apps"
const char *storage_app_directory(storage_volume_t volume);
bool storage_app_locate(const char *id, storage_volume_t *volume);

/* Directory convention, not an access-control boundary for native modules. */
bool storage_app_path(char *out, size_t size, const char *id,
                      storage_app_area_t area, const char *relative);
bool storage_app_available(const char *id);
bool storage_mkdirs(const char *path);
void storage_prepare_directories(void);
uint32_t storage_crc32(const void *data, size_t size);
bool storage_record_load(const char *path, uint32_t magic, uint32_t version, void *data, size_t size);
bool storage_record_save(const char *path, uint32_t magic, uint32_t version, const void *data, size_t size);
/* Serialized replacement. FAT keeps a backup across its two renames; callers
 * opening raw files recover an interrupted replacement before open(). */
bool storage_file_recover(const char *path);
bool storage_file_commit(const char *temporary, const char *path);
bool storage_file_replace(const char *path, const void *data, size_t size);
bool storage_file_import(const char *source, const char *destination);
/* LittleFS has no mtime in this SDK. Content tag replaces it for reader caches. */
bool storage_file_signature(const char *path, uint32_t *signature);

#endif
