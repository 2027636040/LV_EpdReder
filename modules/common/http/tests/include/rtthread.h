#ifndef HTTP_TEST_RTTHREAD_H
#define HTTP_TEST_RTTHREAD_H
#include <stddef.h>
#include <stdint.h>
#define RT_NULL NULL
#define RT_ERROR 1
#define RT_ETIMEOUT 2
#define RT_EFULL 3
#define RT_ENOMEM 4
#define RT_EINVAL 5
#define RT_TICK_PER_SECOND 1000
typedef uint32_t rt_tick_t;
typedef int rt_err_t;
typedef void *rt_event_t;
void *rt_malloc(size_t size);
void *rt_calloc(size_t count, size_t size);
void rt_free(void *pointer);
char *rt_strdup(const char *text);
rt_tick_t rt_tick_get(void);
void rt_thread_mdelay(int ms);
#endif
