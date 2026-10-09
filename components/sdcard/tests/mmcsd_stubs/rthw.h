/* Hardware interrupt API used by the MMC/SD core host tests. */
#ifndef MMCSD_TEST_RTHW_H
#define MMCSD_TEST_RTHW_H

#include <rtthread.h>

rt_base_t rt_hw_interrupt_disable(void);
void rt_hw_interrupt_enable(rt_base_t level);

#endif
