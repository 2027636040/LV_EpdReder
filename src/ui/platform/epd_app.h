#ifndef EPD_APP_H
#define EPD_APP_H

#include "gui_app_fwk.h"
#include "storage_file.h"
#include "app_image.h"
#include "app_service.h"
#include "network.h"
#include "epd_memory.h"

#define EPD_APP_ABI 1u

#ifdef BUILD_DLMODULE
#include "epd_app_profile.h"
#define EPD_APP_DEFINE(app_id) \
    const epd_app_info_t epd_app_info = {EPD_APP_ABI, app_id}; \
    const char epd_app_build_id[] = "EPDAPP:" EPD_APP_BUILD_ID
#endif

/* A module exports this descriptor as epd_app_info and its SDK entry as app_main. */
typedef struct
{
    uint32_t abi;
    const char *id;
} epd_app_info_t;

typedef struct
{
    int battery_percent;
    char clock[16];
} epd_app_status_t;
/* UI-thread platform services; no application-private objects are returned. */
void epd_app_status(epd_app_status_t *status);
void epd_app_set_recent_reading(const char *title, int percent);
void epd_app_request_full_refresh(void);
bool epd_app_open_text_settings(void);
lv_font_t *epd_app_font_create(unsigned family, unsigned size, unsigned weight,
                               uint32_t *identity, lv_font_t **fallback);
void epd_app_font_destroy(lv_font_t *font);
/* UI-thread idle preparation; false means the speculative cache budget is full. */
bool epd_app_font_prepare(const lv_font_t *font, uint32_t letter, bool adjacent);
/* UI-thread operations. Libraries belong to an already-loaded application and
 * must be released after its worker and all library-owned results are stopped. */
void *epd_app_library_open(const char *app_id, const char *name);
void *epd_app_library_symbol(void *library, const char *symbol);
void epd_app_library_close(void *library);
/* Immutable XIP font bytes, valid for the firmware lifetime. No LVGL access is
 * involved, so document workers can use this view. File-only builds return NULL. */
const void *epd_app_builtin_font_data(size_t *size);
const lv_font_t *epd_app_font(void);
typedef enum { EPD_FONT_BODY, EPD_FONT_SMALL, EPD_FONT_TITLE, EPD_FONT_CAPTION,
               EPD_FONT_TEMPERATURE } epd_font_role_t;
const lv_font_t *epd_app_font_role(epd_font_role_t role);
void epd_app_object_init(lv_obj_t *obj);
lv_obj_t *epd_app_label(lv_obj_t *parent, const char *text, int x, int y, int width, const lv_font_t *font);
lv_obj_t *epd_app_centered_label(lv_obj_t *parent, const char *text, int x, int y, int width, const lv_font_t *font);
lv_obj_t *epd_app_panel(lv_obj_t *parent, int x, int y, int width, int height);
lv_obj_t *epd_app_icon(lv_obj_t *parent, const lv_image_dsc_t *src, int x, int y);
/* UI thread only. Copies the original text for change detection; use this
 * function for all subsequent text updates to the same label. */
void epd_app_text(lv_obj_t *label, const char *text);
/* UI thread only. Create once for a screen or modal root. Its DELETE event
 * releases the group; callers must not delete that group themselves.
 * The global keypad selects the active screen's newest visible modal scope. */
lv_group_t *epd_app_input_group(lv_obj_t *owner);
/* Back activates this button; a hidden/disabled button blocks back. */
void epd_app_input_back(lv_obj_t *owner, lv_obj_t *button);
/* Standard buttons join the nearest managed input group automatically. */
lv_obj_t *epd_app_button(lv_obj_t *parent, int x, int y, int width, int height,
                         const char *text, lv_event_cb_t callback, void *data);
/* UI thread only. Header storage is released by the screen's DELETE event. */
void epd_app_header(lv_obj_t *screen);
void epd_app_header_refresh(lv_obj_t *screen);
bool epd_app_refresh_done(void);
void epd_app_clean_draw_buffer(lv_draw_buf_t *buffer);
lv_obj_t *epd_app_back_button(lv_obj_t *screen);
lv_obj_t *epd_app_back_button_cb(lv_obj_t *parent, lv_event_cb_t callback, void *data);

#endif
