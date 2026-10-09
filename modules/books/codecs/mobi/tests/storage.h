#ifndef BOOK_TEST_STORAGE_H
#define BOOK_TEST_STORAGE_H
#include <stdbool.h>
#include <stdint.h>
void storage_lock(void);
void storage_unlock(void);
bool storage_changing(void);
uint32_t storage_revision(void);
#endif
