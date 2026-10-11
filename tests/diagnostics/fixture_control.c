/* SPDX-License-Identifier: Apache-2.0 */
#include <rtthread.h>
#include <rthw.h>
#include <rtm.h>
#include <dlmodule.h>
#include <dlfcn.h>
#include <string.h>
#include <stdlib.h>
#include "fault_probe.h"

static void *s_module;
static void *s_reference;
static int (*s_call)(int);
static volatile int s_pending;
static int s_kind;
static int s_init_kind;
static int s_cleanup_kind;
static int s_nested;

int diag_fixture_phase(const char *phase)
{
    return strcmp(phase, "init") == 0 ? s_init_kind : s_cleanup_kind;
}
RTM_EXPORT(diag_fixture_phase);

static void _run_pending(void *parameter)
{
    (void)parameter;
    s_call(s_kind);
    rt_base_t level = rt_hw_interrupt_disable();
    s_pending = 0;
    rt_hw_interrupt_enable(level);
}

void diag_fixture_poll(void)
{
    if (s_pending == 1) _run_pending(RT_NULL);
}

/* The SDK defines this in kservice.c itself, so a hook exercises diagnostic
 * re-entry without relying on linker wrapping of a same-object call. */
static void _nested_assert(const char *expression, const char *function, rt_size_t line)
{
    (void)expression;
    (void)function;
    (void)line;
    if (s_nested) RT_ASSERT(!s_nested);
}

static void diag_test(int argc, char **argv)
{
    if (argc < 2 || s_pending)
    {
        rt_kprintf("diag_test load|ref PATH; close; unload; exec PATH; phase init|cleanup KIND\n"
                   "diag_test static|dynamic|ui|worker|nested KIND; private PATH KIND\n"
                   "KIND: 0=return 1=assert 2=undefined instruction 3=spin\n");
        return;
    }
    const char *command = argv[1];
    if (!strcmp(command, "phase") && argc == 4)
    {
        if (!strcmp(argv[2], "init")) s_init_kind = atoi(argv[3]);
        else if (!strcmp(argv[2], "cleanup")) s_cleanup_kind = atoi(argv[3]);
    }
    else if (!strcmp(command, "load") && argc == 3 && !s_module)
    {
        s_module = dlopen(argv[2], 0);
        rt_kprintf("[fixture] loaded=%p\n", s_module);
    }
    else if (!strcmp(command, "ref") && argc == 3 && s_module && !s_reference)
    {
        s_reference = dlopen(argv[2], 0);
        rt_kprintf("[fixture] reference=%p\n", s_reference);
    }
    else if (!strcmp(command, "close") && s_reference)
    {
        dlclose(s_reference);
        s_reference = RT_NULL;
    }
    else if (!strcmp(command, "unload") && s_module && !s_reference)
    {
        dlclose(s_module);
        s_module = RT_NULL;
    }
    else if (!strcmp(command, "exec") && argc == 3 && !s_module)
    {
        dlmodule_exec(argv[2], argv[2], strlen(argv[2]));
    }
    else if (!strcmp(command, "private") && argc == 4 && s_module)
    {
        int (*call)(const char *, int) = dlsym(s_module, "diag_module_private");
        if (call) call(argv[2], atoi(argv[3]));
    }
    else if (argc == 3)
    {
        int kind = atoi(argv[2]);
        if (!strcmp(command, "static")) diag_probe(kind);
        else if (!strcmp(command, "nested"))
        {
            s_nested = 1;
            rt_assert_set_hook(_nested_assert);
            diag_probe(1);
        }
        else if (s_module && (!strcmp(command, "dynamic") || !strcmp(command, "ui") ||
                              !strcmp(command, "worker")))
        {
            s_call = dlsym(s_module, "diag_module_call");
            if (!s_call) return;
            s_kind = kind;
            if (!strcmp(command, "dynamic")) s_call(kind);
            else if (!strcmp(command, "ui"))
            {
                rt_base_t level = rt_hw_interrupt_disable();
                s_pending = 1;
                rt_hw_interrupt_enable(level);
            }
            else
            {
                s_pending = 2;
                rt_thread_t thread = rt_thread_create("diagwork", _run_pending, RT_NULL, 2048, 11, 10);
                if (thread) rt_thread_startup(thread);
                else s_pending = 0;
            }
        }
    }
}
MSH_CMD_EXPORT(diag_test, Explicit fault diagnostics fixtures);
