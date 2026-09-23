#ifndef WEATHER_ICONS_H
#define WEATHER_ICONS_H
#include "lvgl.h"
#include <stdbool.h>

#define UI_WEATHER_ICON_COUNT 70
/* QWeather code lookup; large selects 160 px, otherwise 48 px. Unknown codes use 999. */
const lv_image_dsc_t *ui_weather_icon(int code, bool large);
#endif
