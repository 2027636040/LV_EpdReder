#ifndef TXT_TEST_RTTHREAD_H
#define TXT_TEST_RTTHREAD_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
typedef uint32_t rt_tick_t;
typedef int rt_err_t;
typedef size_t rt_size_t;
typedef int rt_bool_t;
typedef void *rt_mutex_t;
#define RT_NULL NULL
#define RT_TRUE 1
#define RT_FALSE 0
#define RT_EOK 0
#define RT_ERROR 1
#define RT_ENOMEM 12
#define RT_EINVAL 22
#define RT_EIO 5
#define RT_WAITING_FOREVER (-1)
#define RT_IPC_FLAG_PRIO 0
#define rt_snprintf snprintf
#define rt_kprintf printf
rt_tick_t rt_tick_get(void);
rt_tick_t rt_tick_from_millisecond(int ms);
rt_mutex_t rt_mutex_create(const char *, int);
int rt_mutex_take(rt_mutex_t, int);
int rt_mutex_release(rt_mutex_t);
int rt_mutex_delete(rt_mutex_t);
#endif
