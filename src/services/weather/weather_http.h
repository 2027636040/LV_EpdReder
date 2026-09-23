#ifndef WEATHER_HTTP_H
#define WEATHER_HTTP_H
#include "weather.h"
bool weather_network_ready(void);
/* Caller frees the bounded, decompressed body with rt_free(). */
char *weather_http_get(const weather_config_t *config, const char *path, int *status);
#endif
