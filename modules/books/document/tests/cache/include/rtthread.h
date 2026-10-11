#ifndef CACHE_TEST_RTTHREAD_H
#define CACHE_TEST_RTTHREAD_H
#include "../../include/rtthread.h"
#include <stdlib.h>
#include <stdio.h>
#define RT_NAME_MAX 32
#define rt_malloc malloc
#define rt_free free
#define rt_snprintf snprintf
#define rt_kprintf printf
#endif
