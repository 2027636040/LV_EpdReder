#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "constants.h"
typedef size_t rt_size_t;
typedef int rt_err_t;
void *content_test_malloc(size_t size);
void content_test_free(void *memory);
#define rt_malloc content_test_malloc
#define rt_free content_test_free
#define rt_snprintf snprintf
#define rt_memcpy memcpy
#define RT_ASSERT assert
#define DBG_LOG 0
#define LOG_I(...) ((void)0)
#define LOG_E(...) ((void)0)
