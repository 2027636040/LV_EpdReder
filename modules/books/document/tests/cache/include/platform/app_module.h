#ifndef CACHE_TEST_APP_MODULE_H
#define CACHE_TEST_APP_MODULE_H
#include "services/storage_file.h"
bool app_module_storage(const char *id, storage_volume_t *volume, uint32_t *session);
#endif
