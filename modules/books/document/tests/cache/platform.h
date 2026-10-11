#ifndef CACHE_TEST_PLATFORM_H
#define CACHE_TEST_PLATFORM_H
#include "services/storage_file.h"
extern unsigned test_allocations, test_io, test_source_opens;
extern int test_fail_alloc, test_fail_write_after, test_fail_rename_at;
extern bool test_fail_sync, test_short_write, test_short_read, test_hotplug;
extern uint32_t test_generation, test_now;
extern storage_volume_t test_app_volume;
extern char test_source[BOOKSHELF_PATH_MAX];
void test_platform_init(const char *directory);
void test_put(const char *path, const void *data, size_t size);
bool test_exists(const char *path);
void test_sparse(const char *path, uint32_t size);
void test_remove(const char *path);
#endif
