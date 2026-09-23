#ifndef LAUNCHER_H
#define LAUNCHER_H
#include "ui_model.h"

typedef struct
{
    char time_text[24];
    ui_radio_state_t bluetooth;
    ui_radio_state_t wifi;
    int battery_percent;
    bool charging;
    char recent_book[128];
    int reading_percent;
    char weather[64];
} launcher_status_t;

typedef enum
{
    LAUNCHER_KEY_PREVIOUS, LAUNCHER_KEY_NEXT, LAUNCHER_KEY_ENTER, LAUNCHER_KEY_BACK
} launcher_key_t;

/* All launcher functions run on the LVGL owner thread. */
bool launcher_init(void);
void launcher_set_status(const launcher_status_t *status);
void launcher_key(launcher_key_t key);
void launcher_process(void);
void launcher_open(ui_page_id_t page);
ui_page_id_t launcher_current_page(void);
unsigned launcher_focus_index(void);

#endif
