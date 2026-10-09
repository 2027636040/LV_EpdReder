#ifndef WORDS_TEST_PLATFORM_H
#define WORDS_TEST_PLATFORM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define STORAGE_PATH_MAX 512
typedef enum { STORAGE_FLASH, STORAGE_SD, STORAGE_COUNT } storage_volume_t;
#define STORAGE_FLASH_APPS "/flash/apps"
#define STORAGE_SD_APPS "/sdcard/.epd/apps"
typedef enum { STORAGE_APP_CODE, STORAGE_APP_DATA, STORAGE_APP_CACHE } storage_app_area_t;
#define EPD_APP_PSRAM 1
#define RT_VER_NUM 0x30103
#define NETUTILS_NTP_TIMEZONE 8
#define RT_WAITING_FOREVER -1
typedef intptr_t rt_base_t;
typedef struct { bool cancelled; } epd_service_t;
typedef struct { void (*run)(epd_service_t *); unsigned stack_size, priority; bool (*flush)(void); } epd_background_t;
typedef struct { bool mounted; uint64_t total, used; } storage_info_t;
void *epd_app_alloc(size_t size, int region);
void *epd_app_realloc(void *pointer, size_t size, int region);
void epd_app_free(void *pointer);
void storage_lock(void);
void storage_unlock(void);
bool storage_available(int volume);
bool storage_path_available(const char *path);
uint32_t storage_card_session(void);
bool storage_session_valid(storage_volume_t volume, uint32_t session);
bool storage_mkdirs(const char *path);
bool storage_app_available(const char *id);
const char *storage_path_root(const char *path);
bool storage_file_recover(const char *path);
bool storage_file_commit(const char *temporary, const char *path);
bool storage_app_path(char *out, size_t size, const char *id, storage_app_area_t area, const char *relative);
bool storage_info(int volume, storage_info_t *info);
uint32_t storage_crc32(const void *data, size_t size);
bool storage_record_save(const char *path, uint32_t magic, uint32_t version, const void *data, size_t size);
bool storage_record_load(const char *path, uint32_t magic, uint32_t version, void *data, size_t size);
rt_base_t rt_hw_interrupt_disable(void);
void rt_hw_interrupt_enable(rt_base_t level);
void epd_service_wake(epd_service_t *service);
bool epd_service_cancelled(epd_service_t *service);
void epd_service_wait(epd_service_t *service, int ticks);
void rt_thread_mdelay(int milliseconds);
void rt_kprintf(const char *format, ...);
epd_service_t *app_service_start(const char *id, const epd_background_t *definition, void *module);
void app_service_stop(const char *id);
#endif
