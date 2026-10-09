#ifndef BOOK_TEST_RTTHREAD_H
#define BOOK_TEST_RTTHREAD_H
#include <stdio.h>
#include <stdint.h>
#include <time.h>
typedef uint32_t rt_tick_t;
#define RT_TICK_PER_SECOND CLOCKS_PER_SEC
static inline rt_tick_t rt_tick_get(void) { return (rt_tick_t)clock(); }
#define rt_kprintf printf
#endif
