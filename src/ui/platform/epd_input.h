#ifndef EPD_INPUT_H
#define EPD_INPUT_H

#include "lvgl.h"
#include <rtthread.h>

typedef struct
{
    uint32_t key;
    uint16_t repeat;
} epd_input_event_t;

/* Optional page semantics. Returning true consumes the operation. */
typedef struct
{
    bool (*ready)(uint32_t key);
    bool (*move)(const epd_input_event_t *events, unsigned count);
    bool (*key)(uint32_t key);
} epd_input_ops_t;

rt_err_t epd_input_init(void);
void epd_input_process(void);
void epd_input_clear(void);
bool epd_input_focusable(lv_obj_t *object);
void epd_input_add_object(lv_obj_t *object);
void epd_input_set_ops(lv_group_t *group, const epd_input_ops_t *ops);

#endif
