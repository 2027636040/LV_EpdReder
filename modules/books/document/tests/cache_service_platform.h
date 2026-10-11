#ifndef CACHE_SERVICE_PLATFORM_H
#define CACHE_SERVICE_PLATFORM_H
#include "platform/epd_app.h"
void *epd_app_library_open(const char *, const char *);
void *epd_app_library_symbol(void *, const char *);
void epd_app_library_close(void *);
void epd_app_clean_draw_buffer(lv_draw_buf_t *);
bool epd_service_cancelled(epd_service_t *);
void epd_service_wake(epd_service_t *);
uint32_t epd_service_wait(epd_service_t *, rt_int32_t);
#endif
