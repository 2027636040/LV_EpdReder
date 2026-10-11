#ifndef READER_TEST_RTTHREAD_H
#define READER_TEST_RTTHREAD_H
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#define RT_WAITING_FOREVER (-1)
#define RT_IPC_FLAG_PRIO 1
struct rt_mutex { unsigned held; };
static unsigned test_mutex_depth;
static inline int rt_mutex_init(struct rt_mutex *mutex, const char *name, int flags)
{ (void)name; (void)flags; mutex->held = 0; return 0; }
static inline int rt_mutex_take(struct rt_mutex *mutex, int timeout)
{ (void)timeout; ++mutex->held; ++test_mutex_depth; return 0; }
static inline int rt_mutex_release(struct rt_mutex *mutex)
{ assert(mutex->held && test_mutex_depth); --mutex->held; --test_mutex_depth; return 0; }
#endif
