/* Host-side RT-Thread substitutes for the MMC/SD core request tests. */
#ifndef MMCSD_TEST_RTTHREAD_H
#define MMCSD_TEST_RTTHREAD_H

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t rt_uint8_t;
typedef uint16_t rt_uint16_t;
typedef uint32_t rt_uint32_t;
typedef int32_t rt_int32_t;
typedef intptr_t rt_base_t;
typedef uintptr_t rt_ubase_t;
typedef int rt_err_t;
typedef int rt_bool_t;

#define RT_NULL NULL
#define RT_EOK 0
#define RT_ERROR 1
#define RT_ETIMEOUT 2
#define RT_EFULL 3
#define RT_ENOMEM 5
#define RT_EBUSY 7
#define RT_EINVAL 10
#define RT_TRUE 1
#define RT_FALSE 0
#define RT_NAME_MAX 16
#define RT_TICK_PER_SECOND 1000
#define RT_THREAD_PRIORITY_MAX 32
#define RT_THREAD_PRIORITY_HIGH 8
#define RT_WAITING_FOREVER (-1)
#define RT_IPC_FLAG_FIFO 0
#define RT_USING_DFS
#define RT_ASSERT assert
#define RTM_EXPORT(name)
#define INIT_PREV_EXPORT(name)
#define rt_inline static inline
#define rt_memset memset
#define rt_memcpy memcpy
#define rt_strncpy strncpy
#define rt_malloc malloc
#define rt_free free
#define rt_kprintf(...) ((void)0)

struct rt_mutex { int nesting; };
struct rt_semaphore { int value; };
struct rt_thread { char name[RT_NAME_MAX]; };
typedef struct rt_thread *rt_thread_t;
struct rt_mailbox
{
    rt_uint32_t *pool;
    unsigned size;
    unsigned entry;
    unsigned in;
    unsigned out;
};

rt_err_t rt_mutex_init(struct rt_mutex *mutex, const char *name, int flag);
rt_err_t rt_mutex_take(struct rt_mutex *mutex, int timeout);
rt_err_t rt_mutex_release(struct rt_mutex *mutex);
rt_err_t rt_mutex_detach(struct rt_mutex *mutex);
rt_err_t rt_sem_init(struct rt_semaphore *sem, const char *name, int value, int flag);
rt_err_t rt_sem_take(struct rt_semaphore *sem, int timeout);
rt_err_t rt_sem_release(struct rt_semaphore *sem);
rt_err_t rt_sem_detach(struct rt_semaphore *sem);
rt_err_t rt_mb_init(struct rt_mailbox *mb, const char *name, void *pool, unsigned size, int flag);
rt_err_t rt_mb_send(struct rt_mailbox *mb, rt_uint32_t value);
rt_err_t rt_mb_recv(struct rt_mailbox *mb, rt_ubase_t *value, int timeout);
rt_err_t rt_thread_init(struct rt_thread *thread, const char *name,
                        void (*entry)(void *), void *param, void *stack,
                        unsigned size, int priority, int tick);
rt_err_t rt_thread_startup(struct rt_thread *thread);
void rt_thread_delay(unsigned ticks);
void mmcsd_test_log(const char *format, ...);

#endif
