/* SPDX-License-Identifier: Apache-2.0 */
#include <rtthread.h>
#include <dlmodule.h>
#include <dlfcn.h>
#include "fault_probe.h"

#ifdef DIAG_PRIVATE
int diag_private_call(int kind)
{
    return diag_probe(kind) + 1;
}
#else
extern int diag_fixture_phase(const char *phase);
typedef int (*probe_fn)(int);

void module_init(struct rt_dlmodule *module)
{
    rt_kprintf("[fixture] init module=%p\n", module);
    diag_probe(diag_fixture_phase("init"));
}

void module_cleanup(struct rt_dlmodule *module)
{
    rt_kprintf("[fixture] cleanup module=%p\n", module);
    diag_probe(diag_fixture_phase("cleanup"));
}

int diag_module_call(int kind)
{
    return diag_probe(kind) + 1;
}

int diag_module_private(const char *path, int kind)
{
    void *library = dlopen(path, 0);
    if (!library) return -RT_ERROR;
    probe_fn call = (probe_fn)dlsym(library, "diag_private_call");
    int result = call ? call(kind) : -RT_ERROR;
    dlclose(library);
    return result;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    /* dlmodule_exec gives this thread its SDK module ownership. */
    return diag_probe(1);
}
#endif
