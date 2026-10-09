#ifndef APP_MODULE_H
#define APP_MODULE_H

#include "storage.h"

/* Module references are opened/closed on the UI thread. An application and its
 * private libraries retain their installation volume and card session until
 * the last module reference is released. */
void *app_module_open(const char *path, const char *id);
void app_module_close(void *module);
bool app_module_loaded(const char *id);
bool app_module_storage(const char *id, storage_volume_t *volume, uint32_t *session);
bool app_modules_present(void);

#endif
