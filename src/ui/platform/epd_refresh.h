#ifndef EPD_REFRESH_H
#define EPD_REFRESH_H

#include "lvgl.h"

/* Install after app_fwk and LVGL initialization, before starting applications. */
void epd_refresh_init(void);

/* UI thread only. Register a status-only region in display coordinates once per
 * screen. Screen deletion releases the registration. */
void epd_refresh_status_area(lv_obj_t *screen, const lv_area_t *area);

#endif
