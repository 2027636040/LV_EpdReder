/* SPDX-License-Identifier: Apache-2.0 */
#ifndef COMMON_READER_H
#define COMMON_READER_H
#include "platform/epd_app.h"
/* The service needs the public position value and worker entry points only. */
typedef struct { uint32_t record, byte; } document_cursor_t;
typedef struct
{
    document_cursor_t cursor;
    uint32_t conversion_version, layout;
    unsigned page, pages, percent;
} reader_position_t;
bool epd_reader_worker_step(epd_service_t *service);
void epd_reader_worker_shutdown(void);
#endif
