/* Deterministic RTOS substitutes for the component tests. */
#ifndef SDCARD_TEST_RTTHREAD_H
#define SDCARD_TEST_RTTHREAD_H
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef intptr_t rt_base_t;
typedef int rt_err_t;
typedef int rt_bool_t;
typedef int rt_tick_t;
#define RT_NULL NULL
#define RT_EOK 0
#define RT_ERROR 1
#define RT_ETIMEOUT 2
#define RT_EFULL 3
#define RT_ENOMEM 5
#define RT_ENOSYS 6
#define RT_EBUSY 7
#define RT_EINVAL 10
#define RT_TRUE 1
#define RT_FALSE 0
#define RT_IPC_FLAG_FIFO 0
#define RT_WAITING_FOREVER (-1)
#define RT_ASSERT assert
#define rt_memset memset
#define rt_strcmp strcmp
#define rt_free free
struct rt_mutex { int nesting; };
struct rt_semaphore { int value; int live; };
struct rt_thread { int live; };
typedef struct rt_mutex *rt_mutex_t;
typedef struct rt_thread *rt_thread_t;
rt_err_t rt_mutex_take(rt_mutex_t mutex, int timeout);
rt_err_t rt_mutex_release(rt_mutex_t mutex);
rt_err_t rt_sem_init(struct rt_semaphore *sem, const char *name, int value, int flag);
rt_err_t rt_sem_detach(struct rt_semaphore *sem);
rt_err_t rt_sem_take(struct rt_semaphore *sem, int timeout);
rt_err_t rt_sem_release(struct rt_semaphore *sem);
rt_thread_t rt_thread_create(const char *, void (*)(void *), void *, unsigned, int, int);
rt_err_t rt_thread_startup(rt_thread_t thread);
rt_err_t rt_thread_delete(rt_thread_t thread);
void rt_thread_mdelay(int milliseconds);
rt_tick_t rt_tick_from_millisecond(int milliseconds);
int rt_get_errno(void);
void rt_set_errno(int error);
#endif
