#ifndef UI_WEATHER_DATA_H
#define UI_WEATHER_DATA_H

#define UI_WEATHER_CITY_COUNT 8
#define UI_WEATHER_METRIC_COUNT 8
#define UI_WEATHER_FORECAST_COUNT 3

typedef struct
{
    int code;
    const char *text;
    const char *temperature_range;
    const char *wind;
} ui_weather_forecast_t;

typedef struct
{
    const char *city;
    const char *region;
    int code;
    int temperature, feels_like, high, low;
    const char *text;
    const char *wind;
    /* Humidity, wind speed, visibility, cloud, sunrise, sunset, pressure, AQI. */
    const char *metrics[UI_WEATHER_METRIC_COUNT];
    ui_weather_forecast_t forecast[UI_WEATHER_FORECAST_COUNT];
} ui_weather_city_t;

const ui_weather_city_t *ui_weather_city(unsigned index);

#endif
