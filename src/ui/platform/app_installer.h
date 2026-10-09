#ifndef EPD_APP_INSTALLER_H
#define EPD_APP_INSTALLER_H
#include <stdbool.h>
#include <stdint.h>
#include "storage.h"

typedef enum { APP_JOB_IDLE, APP_JOB_RUNNING, APP_JOB_DONE, APP_JOB_FAILED } app_job_state_t;
typedef struct { uint32_t ticket; app_job_state_t state; char message[96]; } app_job_result_t;
bool app_installer_start(const char *id, bool uninstall, storage_volume_t volume, uint32_t *ticket);
void app_installer_result(app_job_result_t *result);
bool app_installer_busy(void);
void app_installer_recover(void);
void app_installer_process(void);
bool app_installer_blocks(const char *id);
bool app_installer_volume_ready(storage_volume_t volume);
void app_installer_storage_changed(void);
#endif
