#ifndef TEST_RTTHREAD_H
#define TEST_RTTHREAD_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef uint32_t rt_tick_t;
typedef int rt_err_t;
typedef unsigned rt_size_t;
typedef long rt_base_t;
typedef int32_t rt_int32_t;
#define RT_WAITING_FOREVER (-1)
#define RT_ALIGN(v, a) (((v) + (a) - 1) & ~((uintptr_t)(a) - 1))
rt_tick_t rt_tick_get(void);
rt_tick_t rt_tick_from_millisecond(int ms);
#endif
