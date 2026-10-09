/* SPDX-License-Identifier: Apache-2.0 */
#include "weather_ui.h"
#include "weather_history.h"

#define HISTORY_ROWS 6

typedef struct
{
    unsigned first, total;
    weather_city_t cities[HISTORY_ROWS];
    lv_obj_t *rows[HISTORY_ROWS], *names[HISTORY_ROWS], *details[HISTORY_ROWS];
    lv_obj_t *selected[HISTORY_ROWS];
    lv_obj_t *empty, *pager, *previous, *next;
} history_view_t;

static void history_visible(lv_obj_t *object, bool visible)
{
    if (visible) lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}

void city_history_resume(void)
{
    history_view_t *view = current->history_view;
    if (!view) return;
    view->total = weather_history_page(view->first, view->cities, HISTORY_ROWS);
    if (view->first && view->first >= view->total)
    {
        view->first = 0;
        view->total = weather_history_page(0, view->cities, HISTORY_ROWS);
    }
    weather_config_t config;
    weather_get_config(&config);
    for (unsigned row = 0; row < HISTORY_ROWS; ++row)
    {
        const weather_city_t *city = &view->cities[row];
        bool visible = city->city_id[0] != 0;
        history_visible(view->rows[row], visible);
        if (!visible) continue;
        char detail[112];
        snprintf(detail, sizeof(detail), "%s%s%s", city->region,
                 city->region[0] ? " · " : "", city->city_id);
        text_update(view->names[row], city->city);
        text_update(view->details[row], detail);
        text_update(view->selected[row], !strcmp(config.city_id, city->city_id) ? "当前" : "");
    }
    history_visible(view->empty, view->total == 0);
    history_visible(view->previous, view->first > 0);
    history_visible(view->next, view->first + HISTORY_ROWS < view->total);
    unsigned pages = (view->total + HISTORY_ROWS - 1) / HISTORY_ROWS;
    char text[32];
    snprintf(text, sizeof(text), "%u / %u", view->first / HISTORY_ROWS + 1, pages);
    text_update(view->pager, pages > 1 ? text : "");
    if (view->total) lv_group_focus_obj(view->rows[0]);
}

void city_history_create(lv_obj_t *screen)
{
    page_title_create(screen, "查询历史", ui_font_title());
    history_view_t *view = lv_malloc_zeroed(sizeof(*view));
    current->history_view = view;
    if (!view)
    {
        centered_label(screen, "内存不足", MARGIN, 490, CONTENT_WIDTH, ui_font_body());
        return;
    }
    for (unsigned row = 0; row < HISTORY_ROWS; ++row)
    {
        lv_obj_t *button = button_create(screen, MARGIN, 208 + row * 132, CONTENT_WIDTH, 116,
                                         NULL, HISTORY_CITY_BASE + (int)row);
        view->rows[row] = button;
        view->names[row] = label_create(button, "", 20, 16, CONTENT_WIDTH - 132, ui_font_title());
        view->selected[row] = centered_label(button, "", CONTENT_WIDTH - 104, 24, 84, ui_font_small());
        view->details[row] = label_create(button, "", 20, 70, CONTENT_WIDTH - 40, ui_font_caption());
    }
    view->empty = centered_label(screen, "暂无查询历史", MARGIN, 490, CONTENT_WIDTH, ui_font_body());
    view->pager = centered_label(screen, "", MARGIN, 1044, CONTENT_WIDTH, ui_font_small());
    view->previous = button_create(screen, MARGIN, 1100, 288, 76, "上一页", HISTORY_PREVIOUS);
    view->next = button_create(screen, WIDTH - MARGIN - 288, 1100, 288, 76, "下一页", HISTORY_NEXT);
}

void city_history_stop(void)
{
    lv_free(current->history_view);
    current->history_view = NULL;
}

bool city_history_action(int target)
{
    if (current->id != UI_PAGE_CITY_HISTORY || !current->history_view) return false;
    history_view_t *view = current->history_view;
    if (target == HISTORY_PREVIOUS)
    {
        if (view->first >= HISTORY_ROWS) view->first -= HISTORY_ROWS;
        city_history_resume();
    }
    else if (target == HISTORY_NEXT)
    {
        if (view->first + HISTORY_ROWS < view->total) view->first += HISTORY_ROWS;
        city_history_resume();
    }
    else if (target >= HISTORY_CITY_BASE && target < HISTORY_CITY_BASE + HISTORY_ROWS)
    {
        const weather_city_t *city = &view->cities[target - HISTORY_CITY_BASE];
        if (city->city_id[0])
        {
            snprintf(current->city_digits, sizeof(current->city_digits), "%s", city->city_id);
            weather_action_start(WEATHER_JOB_HISTORY_CITY);
        }
    }
    else return false;
    return true;
}
