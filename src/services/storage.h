#ifndef EPD_STORAGE_H
#define EPD_STORAGE_H
#include <rtthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define STORAGE_FLASH_ROOT "/flash"
#define STORAGE_SD_ROOT "/sdcard"
#define STORAGE_PATH_MAX 512
typedef enum { STORAGE_FLASH, STORAGE_SD, STORAGE_COUNT } storage_volume_t;
typedef struct { bool mounted; uint64_t total, used; } storage_info_t;

void storage_init(void);
const char *storage_root(storage_volume_t volume);
const char *storage_path_root(const char *path);
const char *storage_relative_path(const char *path);
bool storage_info(storage_volume_t volume, storage_info_t *info);
bool storage_available(storage_volume_t volume);
bool storage_path_available(const char *path);
/* A removed card invalidates its session before a replacement can be mounted. */
uint32_t storage_card_session(void);
bool storage_session_valid(storage_volume_t volume, uint32_t session);
bool storage_apps_enabled(storage_volume_t volume);
bool storage_card_enable_apps(uint32_t session);
uint32_t storage_revision(void);
/* The UI owner closes its persistent files/fonts before acknowledging removal. */
bool storage_release_requested(void);
uint32_t storage_release_request_id(void);
rt_err_t storage_release_complete(uint32_t release_id);
bool storage_changing(void);
bool storage_release_timed_out(void);
void storage_lock(void);
void storage_unlock(void);
void storage_format_size(uint64_t bytes, char *text, size_t size);
#endif
