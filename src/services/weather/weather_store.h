#ifndef WEATHER_STORE_H
#define WEATHER_STORE_H
#include "weather.h"
/* Called by the weather worker only, except for loading before its startup. */
bool weather_store_load(weather_config_t *config, weather_info_t *info);
bool weather_store_save(const weather_config_t *config, const weather_info_t *info);
bool weather_store_import(weather_config_t *config, char *error, size_t size);
bool weather_config_valid(const weather_config_t *config);
#endif
