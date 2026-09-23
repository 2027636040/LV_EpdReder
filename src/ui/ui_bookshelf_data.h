#ifndef UI_BOOKSHELF_DATA_H
#define UI_BOOKSHELF_DATA_H

#include "bookshelf.h"
#include <stddef.h>

typedef struct
{
    uint32_t offset, current_page, total_pages, layout, progress, last_read;
} ui_reading_position_t;

typedef struct
{
    bookshelf_item_t file;
    ui_reading_position_t position;
    uint32_t order;
    char last_read[48];
} ui_bookshelf_book_t;

int ui_bookshelf_refresh(void);
unsigned ui_bookshelf_count(void);
const ui_bookshelf_book_t *ui_bookshelf_book(unsigned index);
bool ui_bookshelf_save(unsigned index, const ui_reading_position_t *position);
void ui_bookshelf_cache_path(unsigned index, char *out, size_t size, const char *suffix);
bool ui_bookshelf_storage_ready(void);

#endif
