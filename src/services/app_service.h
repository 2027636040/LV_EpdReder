#ifndef EPD_APP_SERVICE_H
#define EPD_APP_SERVICE_H
#include <rtthread.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct epd_service epd_service_t;
#define EPD_SERVICE_WAKE 1u
#define EPD_SERVICE_NETWORK 2u
#define EPD_SERVICE_STOP 4u
typedef struct
{
    void (*run)(epd_service_t *service);
    uint32_t stack_size;
    uint8_t priority;
    bool network;
    int (*command)(int argc, char **argv);
    /* One bounded UI-thread step, optionally alongside run. stopping becomes
     * true only after run and shell calls have returned. Return true only after
     * page teardown and all UI-owned resources have been released. */
    bool (*ui_process)(epd_service_t *service, bool stopping);
    /* Optional UI-thread persistence barrier before a controlled power-off. */
    bool (*flush)(void);
} epd_background_t;

/* A module exports epd_app_background. Its run function must return on stop,
 * close its requests/handles, and never retain LVGL page objects. */
uint32_t epd_service_wait(epd_service_t *service, rt_int32_t ticks);
bool epd_service_cancelled(epd_service_t *service);
void epd_service_wake(epd_service_t *service);
/* Current UI callback only. Request the next step after delay_ms; repeated
 * requests keep the earliest deadline. Consumed before the next callback and
 * cleared on stop. Waiting for data/display needs no request. */
void epd_service_ui_request(epd_service_t *service, uint32_t delay_ms);
/* The worker publishes a copy; the UI reads it without calling module code. */
void epd_service_publish_summary(epd_service_t *service, const char *text);
/* Resident snapshots: no module callback or borrowed pointer. The revision
 * changes on publication/removal, not on repeated identical publications. */
uint32_t epd_app_summary_revision(void);
bool epd_app_summary_read(const char *id, char *text, size_t capacity);
/* Optional shell command; holds the module alive until the call returns. */
int app_service_command(const char *id, int argc, char **argv);

/* UI/platform owner only. Module lifetime and thread completion are distinct. */
epd_service_t *app_service_start(const char *id, const epd_background_t *definition, void *module);
void app_service_stop(const char *id);
bool app_service_running(const char *id);
bool app_service_stopping(void);
void app_service_process(void);
/* UI thread: min(default_ms, remaining service deadlines), in milliseconds.
 * Expired deadlines return zero. Reading does not consume a request. */
uint32_t app_service_ui_delay(uint32_t default_ms);
bool app_service_flush(void);
void app_service_stop_modules(void);
bool app_service_modules_running(void);
#endif
