#ifndef RECOVERY_TEST_GUI_APP_FWK_H
#define RECOVERY_TEST_GUI_APP_FWK_H
#include <stdint.h>
#include <stdbool.h>
#define GUI_APP_ID_MAX_LEN 32
typedef struct { uint32_t user_data, entry_func; } app_entity_info;
typedef struct { const char *id, *name; const void *icon; } builtin_app_desc_t;
typedef struct { char id[GUI_APP_ID_MAX_LEN]; } gui_runing_app_t;
typedef void *intent_t;
#endif
