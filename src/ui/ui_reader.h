#ifndef UI_READER_H
#define UI_READER_H

#include "lvgl.h"
#include <stdbool.h>

typedef struct
{
    const char *text;
    const lv_font_t *font;
    const char *title;
    unsigned page, pages, percent, margin;
    int line_space;
    uint32_t revision;
    bool ready, indexing, saved;
    char error[96];
} ui_reader_view_t;

bool ui_reader_open(unsigned book);
void ui_reader_close(void);
void ui_reader_save(void);
bool ui_reader_reflow(void);
void ui_reader_process(void);
void ui_reader_seek(unsigned page);
void ui_reader_turn(int direction);
const ui_reader_view_t *ui_reader_view(void);
bool ui_reader_waiting(void);

#define UI_READER_TOP 64
#define UI_READER_BOTTOM 1152

#endif
