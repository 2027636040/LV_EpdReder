#ifndef UI_APP_H
#define UI_APP_H
#include <rtthread.h>
#include <stdbool.h>

rt_err_t ui_app_init(void);
void ui_app_process(void);

/* Service callbacks may publish data from another thread. No LVGL calls occur here. */
void ui_app_set_wifi(bool enabled, bool connected);
void ui_app_set_recent_reading(const char *title, int percent);
void ui_app_set_weather_summary(const char *summary);

#endif
