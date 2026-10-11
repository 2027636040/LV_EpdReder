#ifndef BOARD_HOST_RTTHREAD_H
#define BOARD_HOST_RTTHREAD_H
#include <stdlib.h>
#include <string.h>
#define rt_memcpy memcpy
#define rt_memset memset
#ifdef BOARD_CJSON_FAULTS
void *board_json_alloc(size_t size);
void *board_json_realloc(void *pointer, size_t size);
void board_json_free(void *pointer);
/* Only the separately compiled SDK cJSON translation unit includes this shim. */
#define malloc board_json_alloc
#define realloc board_json_realloc
#define free board_json_free
#endif
#endif
