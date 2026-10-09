#ifndef UI_READER_H
#define UI_READER_H

#include "lvgl.h"
#include <stdbool.h>
#include <limits.h>

/* Navigation computes page deltas with signed int. */
#define UI_READER_PAGE_MAX ((unsigned)INT_MAX - 1u)

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

/* Prepare resources without activating a page or changing reading history. */
bool ui_reader_open(unsigned book);
void ui_reader_close(void);
bool ui_reader_save(void);
bool ui_reader_reflow(void);
bool ui_reader_settings_changed(void);
/* Advance pagination without changing the visible page or reading position. */
void ui_reader_index_process(void);
/* Bounded UI-idle preparation, also usable while the panel transmits a frame. */
void ui_reader_prepare_process(void);
/* Cancel a queued seek while retaining the last visible page or restore anchor. */
void ui_reader_cancel_pending(void);
/* Foreground only: advance pagination, activate the requested page and save it. */
void ui_reader_process(void);
void ui_reader_seek(unsigned page);
void ui_reader_turn(int direction);
const ui_reader_view_t *ui_reader_view(void);
bool ui_reader_waiting(void);

#define UI_READER_TOP 64
#define UI_READER_BOTTOM 1152

#endif
