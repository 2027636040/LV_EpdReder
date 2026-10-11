#ifndef RECOVERY_TEST_RTTHREAD_H
#define RECOVERY_TEST_RTTHREAD_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
typedef int rt_err_t;
typedef int rt_base_t;
typedef int32_t rt_int32_t;
typedef struct { int unused; } rt_list_t;
#define RT_NAME_MAX 32
#define RT_EOK 0
#define RT_EBUSY 7
#define RT_EIO 8
#define RT_EINVAL 10
#define RT_ENOSYS 6
#define RTM_EXPORT(symbol)
#define rt_malloc malloc
#define rt_free free
#define rt_snprintf snprintf
rt_base_t rt_hw_interrupt_disable(void);
void rt_hw_interrupt_enable(rt_base_t level);
void rt_set_errno(rt_err_t error);
void rt_kprintf(const char *format, ...);
#endif
