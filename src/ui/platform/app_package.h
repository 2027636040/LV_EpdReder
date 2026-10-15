#ifndef EPD_APP_PACKAGE_H
#define EPD_APP_PACKAGE_H

#include "app_catalog.h"

#define EPD_PACKAGE_FORMAT 1u
#define EPD_PACKAGE_PATH_MAX 160

typedef enum
{
    APP_PACKAGE_OK,
    APP_PACKAGE_FORMAT_ERROR,
    APP_PACKAGE_INCOMPATIBLE,
    APP_PACKAGE_IO_ERROR,
    APP_PACKAGE_INTEGRITY_ERROR
} app_package_status_t;

/* Callers hold storage_lock; these functions never load or execute a module. */
bool app_package_metadata(const char *root, const char *id, epd_app_entry_t *entry);
bool app_package_handles_path(const char *root, const char *path);
/* Optional load_peak is the main ELF file plus its LOAD span, for reclaiming
 * cache before dlopen. It is not an allocation admission limit. */
app_package_status_t app_package_verify(const char *root, const char *id, bool all_files,
                                        size_t *load_peak);
app_package_status_t app_package_verify_library(const char *root, const char *id,
                                               const char *name, size_t *load_peak);
const char *app_package_error(app_package_status_t status);

#endif
