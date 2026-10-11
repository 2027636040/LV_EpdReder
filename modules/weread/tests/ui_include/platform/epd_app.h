/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WEREAD_UI_TEST_PLATFORM_H
#define WEREAD_UI_TEST_PLATFORM_H
#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>
#define EPD_APP_DEFINE(id)
#define EPD_APP_PSRAM 1
#define STORAGE_PATH_MAX 512
typedef struct epd_service epd_service_t;
typedef enum { STORAGE_APP_CODE, STORAGE_APP_DATA, STORAGE_APP_CACHE } storage_app_area_t;
typedef enum { EPD_FONT_BODY, EPD_FONT_SMALL, EPD_FONT_TITLE, EPD_FONT_CAPTION,
               EPD_FONT_TEMPERATURE } epd_font_role_t;
typedef struct { int battery_percent; char clock[16]; } epd_app_status_t;
typedef void *intent_t;
typedef enum { GUI_APP_MSG_ONSTART, GUI_APP_MSG_ONRESUME,
               GUI_APP_MSG_ONPAUSE, GUI_APP_MSG_ONSTOP } gui_app_msg_type_t;
typedef void (*gui_page_msg_cb_t)(gui_app_msg_type_t, void *);
void *gui_app_this_page_memory(void);
void *gui_app_this_page_userdata(void);
int gui_app_create_page_for_app_ext(const char *, const char *, gui_page_msg_cb_t, void *, uint32_t);
int gui_app_regist_msg_handler_ext(const char *, gui_page_msg_cb_t, void *, uint32_t);
void gui_app_exec_now(void);
void gui_app_close_anim(void);
void gui_app_goback(void);
void *epd_app_alloc(size_t size, int region);
void epd_app_free(void *memory);
const lv_font_t *epd_app_font(void);
const lv_font_t *epd_app_font_role(epd_font_role_t role);
void epd_app_object_init(lv_obj_t *object);
lv_group_t *epd_app_input_group(lv_obj_t *owner);
void epd_app_input_back(lv_obj_t *owner, lv_obj_t *button);
void epd_app_header(lv_obj_t *screen);
void epd_app_header_refresh(lv_obj_t *screen);
lv_obj_t *epd_app_back_button(lv_obj_t *screen);
lv_obj_t *epd_app_label(lv_obj_t *, const char *, int, int, int, const lv_font_t *);
lv_obj_t *epd_app_centered_label(lv_obj_t *, const char *, int, int, int, const lv_font_t *);
lv_obj_t *epd_app_panel(lv_obj_t *, int, int, int, int);
lv_obj_t *epd_app_button(lv_obj_t *, int, int, int, int, const char *, lv_event_cb_t, void *);
void epd_app_text(lv_obj_t *, const char *);
bool epd_app_open_text_settings(void);
bool epd_app_refresh_done(void);
void epd_app_status(epd_app_status_t *status);
void epd_service_ui_request(epd_service_t *service, unsigned delay);
#endif
