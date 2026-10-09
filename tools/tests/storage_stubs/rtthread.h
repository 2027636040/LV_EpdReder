#include "../../../components/sdcard/tests/service_stubs/rtthread.h"
#include <stdio.h>
typedef uint8_t rt_uint8_t;
#define RT_IPC_FLAG_PRIO 1
#define RTM_EXPORT(symbol)
#define INIT_PREV_EXPORT(symbol) int test_board_early_init(void) { return symbol(); }
#define rt_snprintf snprintf
rt_err_t rt_mutex_init(rt_mutex_t mutex, const char *name, int flag);
void rt_kprintf(const char *format, ...);
