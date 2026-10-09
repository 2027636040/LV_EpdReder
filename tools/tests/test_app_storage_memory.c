#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../src/ui/platform/app_memory_usage.h"

#define RT_EOK 0
#define RT_WAITING_FOREVER (-1)
#define RT_Object_Class_MemHeap 1
#define HPSYS_RAM0_BASE ((uintptr_t)0x20000000u)
#define HPSYS_RAM_SIZE 1024u
unsigned char __RW_PSRAM_NON_RET_end__[1];
#define PSRAM_BASE ((uintptr_t)__RW_PSRAM_NON_RET_end__ - 1536u)
#define PSRAM_SIZE 2048u
struct rt_memheap { void *start_addr; size_t available_size; int lock; };
struct rt_memheap app_image_psram_memheap;
static struct rt_memheap mock_system_heap, mock_ui_heap;
static bool missing_heap;
static unsigned takes, releases;
static int rt_sem_take(int *lock, int timeout)
{ assert(timeout == RT_WAITING_FOREVER); if (*lock) return -1; ++takes; return RT_EOK; }
static void rt_sem_release(int *lock) { assert(!*lock); ++releases; }
static void *rt_object_find(const char *name, int type)
{
    assert(type == RT_Object_Class_MemHeap);
    if (!strcmp(name, "heap")) return &mock_system_heap;
    assert(!strcmp(name, "lv_psram"));
    return missing_heap ? NULL : &mock_ui_heap;
}

#include "memory_under_test.h"

static void assert_empty(const app_memory_snapshot_t *snapshot)
{
    app_memory_snapshot_t empty = {0};
    assert(!memcmp(snapshot, &empty, sizeof(empty)));
    assert(takes == releases);
}

int main(void)
{
    mock_system_heap = (struct rt_memheap){(void *)(HPSYS_RAM0_BASE + 64), 256, 0};
    app_image_psram_memheap = (struct rt_memheap){(void *)PSRAM_BASE, 512, 0};
    mock_ui_heap = (struct rt_memheap){(void *)(PSRAM_BASE + 1024), 128, 0};
    app_memory_snapshot_t snapshot;
    memset(&snapshot, 0xff, sizeof(snapshot));
    assert(app_memory_snapshot(&snapshot));
    assert(snapshot.ram[0].total == 1024 && snapshot.ram[0].used == 768);
    assert(snapshot.ram[0].available == 256 && snapshot.ram[0].unassigned == 0);
    assert(snapshot.ram[1].total == 2048 && snapshot.ram[1].used == 896);
    assert(snapshot.ram[1].available == 640 && snapshot.ram[1].unassigned == 512);
    assert(takes == 3 && releases == 3);
    app_image_psram_memheap.available_size = 768;
    assert(app_memory_snapshot(&snapshot));
    assert(snapshot.ram[1].used == 640 && snapshot.ram[1].available == 896);
    missing_heap = true;
    assert(!app_memory_snapshot(&snapshot));
    assert_empty(&snapshot);
    missing_heap = false;
    mock_ui_heap.lock = 1;
    assert(!app_memory_snapshot(&snapshot));
    assert_empty(&snapshot);
    mock_ui_heap.lock = 0;
    mock_ui_heap.start_addr = NULL;
    assert(!app_memory_snapshot(&snapshot));
    assert_empty(&snapshot);
    mock_ui_heap.start_addr = (void *)(PSRAM_BASE + 1024);
    mock_system_heap.available_size = HPSYS_RAM_SIZE + 1;
    assert(!app_memory_snapshot(&snapshot));
    assert_empty(&snapshot);
    puts("memory: RAM totals, refresh, missing heap, lock failure and invalid totals passed");
    return 0;
}
