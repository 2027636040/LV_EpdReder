#include "epd_refresh.h"
#include "gui_app_int.h"
#include "lvgl.h"
#include "epd_waveform.h"   /* 屏驱波形接口（SDK: customer/peripherals/display/epd_e0470a03）*/

static lv_obj_t *foreground_screen;
static lv_obj_t *submitted_screen;
static lv_obj_t *rendering_screen;
static lv_area_t rendering_status_area;
static bool status_only;

typedef struct status_region
{
    struct status_region *next;
    lv_obj_t *screen;
    lv_area_t area;
} status_region_t;

static status_region_t *status_regions;

static void status_region_deleted(lv_event_t *event)
{
    status_region_t *region = lv_event_get_user_data(event);
    status_region_t **link = &status_regions;
    while (*link != region) link = &(*link)->next;
    *link = region->next;
    lv_free(region);
}

void epd_refresh_status_area(lv_obj_t *screen, const lv_area_t *area)
{
    status_region_t *region = lv_malloc(sizeof(*region));
    LV_ASSERT_MALLOC(region);
    if (!region) return;
    region->screen = screen;
    region->area = *area;
    region->next = status_regions;
    status_regions = region;
    lv_obj_add_event_cb(screen, status_region_deleted, LV_EVENT_DELETE, region);
}

static void page_lifecycle(const screen_t screen, gui_app_msg_type_t message)
{
    lv_obj_t *object = (lv_obj_t *)screen;
    if (message == GUI_APP_MSG_ONRESUME)
    {
        foreground_screen = object;
    }
    else if (message == GUI_APP_MSG_ONSTOP)
    {
        /* Framework screen addresses can be reused by a later page. */
        if (foreground_screen == object) foreground_screen = NULL;
        if (submitted_screen == object) submitted_screen = NULL;
        if (rendering_screen == object) rendering_screen = NULL;
    }
}

static void display_event(lv_event_t *event)
{
    lv_display_t *display = lv_event_get_target(event);
    if (lv_event_get_code(event) == LV_EVENT_REFR_START)
    {
        /* ONSTART temporarily loads screens that have not entered the foreground. */
        lv_obj_t *screen = lv_display_get_screen_active(display);
        rendering_screen = screen == foreground_screen ? screen : NULL;
        status_only = false;
        for (status_region_t *region = status_regions; region; region = region->next)
        {
            if (region->screen != rendering_screen) continue;
            rendering_status_area = region->area;
            status_only = true;
            break;
        }
    }
    else
    {
        const lv_area_t *area = lv_event_get_param(event);
        if (area->x1 > area->x2 || area->y1 > area->y2) return;

        /* Check every chunk: content and status can share one LCD frame. */
        if (area->x1 < rendering_status_area.x1 || area->y1 < rendering_status_area.y1 ||
            area->x2 > rendering_status_area.x2 || area->y2 > rendering_status_area.y2)
            status_only = false;
        if (!lv_display_flush_is_last(display)) return;

        /* Explicit partial refresh leaves the periodic counter unchanged. */
        EpdDrawMode mode = status_only ? EPD_DRAW_MODE_PARTIAL : EPD_DRAW_MODE_AUTO;
        if (rendering_screen && rendering_screen != submitted_screen) mode = EPD_DRAW_MODE_FULL;
        /* Only the last LVGL chunk sends the assembled LCD framebuffer. */
        epd_wave_submit_frame(mode);
        submitted_screen = rendering_screen;
    }
}

void epd_refresh_init(void)
{
    /* With APP_TRANS_ANIMATION_NONE, the SDK does not occupy this lifecycle hook. */
    app_schedule_set_anim_hook(page_lifecycle);
    lv_display_t *display = lv_display_get_default();
    lv_display_add_event_cb(display, display_event, LV_EVENT_REFR_START, NULL);
    lv_display_add_event_cb(display, display_event, LV_EVENT_FLUSH_START, NULL);
}
