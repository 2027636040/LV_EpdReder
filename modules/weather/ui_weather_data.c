#include "ui_weather_data.h"
#include "weather.h"
#include <stdio.h>
#include <string.h>

static ui_weather_view_t view;

static void value_text(char *buffer, size_t size, int value, const char *unit)
{
    if (value == WEATHER_MISSING) snprintf(buffer, size, "--");
    else snprintf(buffer, size, "%d%s", value, unit);
}

bool ui_weather_process(void)
{
    weather_info_t data;
    weather_get_info(&data);
    if (view.revision && data.revision == view.revision) return false;
    memset(&view, 0, sizeof(view));
    view.revision = data.revision;
    view.code = data.valid ? data.code : 999;
    snprintf(view.city, sizeof(view.city), "%s", data.city[0] ? data.city : "天气");
    snprintf(view.temperature, sizeof(view.temperature), "--℃");
    snprintf(view.description, sizeof(view.description), "暂无天气数据");
    snprintf(view.range_wind, sizeof(view.range_wind), "H:--℃   L:--℃   风：--");
    snprintf(view.update_time, sizeof(view.update_time), "更新: --");
    snprintf(view.summary, sizeof(view.summary), "暂无天气数据");
    snprintf(view.source, sizeof(view.source), "数据来源：和风天气");
    for (unsigned i = 0; i < UI_WEATHER_METRIC_COUNT; ++i)
        snprintf(view.metrics[i], sizeof(view.metrics[i]), "--");
    for (unsigned i = 0; i < UI_WEATHER_FORECAST_COUNT; ++i)
    {
        ui_weather_forecast_t *f = &view.forecast[i];
        f->code = 999;
        snprintf(f->date, sizeof(f->date), "--");
        snprintf(f->text, sizeof(f->text), "--");
        snprintf(f->temperature_range, sizeof(f->temperature_range), "--");
        snprintf(f->wind, sizeof(f->wind), "--");
    }
    if (!data.valid) return true;
    char feels[24], high[24], low[24];
    value_text(view.temperature, sizeof(view.temperature), data.temperature, "℃");
    value_text(feels, sizeof(feels), data.feels_like, "℃");
    value_text(high, sizeof(high), data.high, "℃");
    value_text(low, sizeof(low), data.low, "℃");
    snprintf(view.description, sizeof(view.description), "%s | 体感 %s", data.text, feels);
    snprintf(view.range_wind, sizeof(view.range_wind), "H:%s   L:%s   %s %s级", high, low,
             data.wind_dir[0] ? data.wind_dir : "风", data.wind_scale_text[0] ? data.wind_scale_text : "--");
    const char *time = strchr(data.update_time, 'T');
    if (time && strlen(time + 1) >= 5)
        snprintf(view.update_time, sizeof(view.update_time), "更新: %.5s", time + 1);
    snprintf(view.summary, sizeof(view.summary), "%s %s %s", data.city, data.text, view.temperature);
    snprintf(view.source, sizeof(view.source), "和风天气 | %s", data.region);
    value_text(view.metrics[0], sizeof(view.metrics[0]), data.humidity, "%");
    value_text(view.metrics[1], sizeof(view.metrics[1]), data.wind_speed, " km/h");
    value_text(view.metrics[2], sizeof(view.metrics[2]), data.visibility, " km");
    value_text(view.metrics[3], sizeof(view.metrics[3]), data.cloud, "%");
    snprintf(view.metrics[4], sizeof(view.metrics[4]), "%s", data.sunrise[0] ? data.sunrise : "--");
    snprintf(view.metrics[5], sizeof(view.metrics[5]), "%s", data.sunset[0] ? data.sunset : "--");
    value_text(view.metrics[6], sizeof(view.metrics[6]), data.pressure, " hPa");
    if (data.aqi >= 0)
        snprintf(view.metrics[7], sizeof(view.metrics[7]), "%d %s", data.aqi, data.aqi_category);
    for (unsigned i = 0; i < (unsigned)data.forecast_count && i < UI_WEATHER_FORECAST_COUNT; ++i)
    {
        const weather_forecast_t *src = &data.forecast[i];
        ui_weather_forecast_t *dst = &view.forecast[i];
        dst->code = src->code;
        snprintf(dst->date, sizeof(dst->date), "%s", strlen(src->date) == 10 ? src->date + 5 : src->date);
        snprintf(dst->text, sizeof(dst->text), "%s", src->text);
        snprintf(dst->temperature_range, sizeof(dst->temperature_range), "%d℃~%d℃", src->low, src->high);
        snprintf(dst->wind, sizeof(dst->wind), "%s %s级", src->wind_dir,
                 src->wind_scale_text[0] ? src->wind_scale_text : "--");
    }
    return true;
}

const ui_weather_view_t *ui_weather_view(void)
{
    return &view;
}
