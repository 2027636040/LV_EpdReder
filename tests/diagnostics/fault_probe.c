/* SPDX-License-Identifier: Apache-2.0 */
#include <rtthread.h>
#include "fault_probe.h"

/* Compile the same source into the firmware and both test modules. */
__attribute__((noinline)) int diag_probe(int kind)
{
    volatile int result = kind;
    if (kind == 1)
    {
        RT_ASSERT(kind != 1);
    }
    else if (kind == 2)
    {
        __asm volatile ("udf #0");
    }
    else if (kind == 3)
    {
        while (result == 3) __asm volatile ("nop");
    }
    return result;
}
