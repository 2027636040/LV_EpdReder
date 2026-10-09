#include "ui_internal.h"
#include "platform/app_memory_usage.h"
#include "storage.h"

#define MEMORY_REFRESH (-560)

typedef struct
{
    app_memory_snapshot_t snapshot;
    bool valid;
    lv_obj_t *usage[2], *available[2], *unassigned[2];
} memory_view_t;

static void memory_render(void)
{
    memory_view_t *view = current->feature;
    char text[96], used[24], total[24];
    for (unsigned r = 0; r < 2; ++r)
    {
        const app_ram_usage_t *ram = &view->snapshot.ram[r];
        if (!view->valid)
        {
            text_update(view->usage[r], "读取失败");
            text_update(view->available[r], "");
            text_update(view->unassigned[r], "");
            continue;
        }
        storage_format_size(ram->used, used, sizeof(used));
        storage_format_size(ram->total, total, sizeof(total));
        snprintf(text, sizeof(text), "已用 %s / 共 %s", used, total);
        text_update(view->usage[r], text);
        storage_format_size(ram->available, used, sizeof(used));
        snprintf(text, sizeof(text), "可分配 %s", used);
        text_update(view->available[r], text);
        storage_format_size(ram->unassigned, used, sizeof(used));
        snprintf(text, sizeof(text), "未分配 %s", used);
        text_update(view->unassigned[r], ram->unassigned ? text : "");
    }
}

void memory_page_resume(void)
{
    memory_view_t *view = current->feature;
    if (!view) return;
    view->valid = app_memory_snapshot(&view->snapshot);
    memory_render();
}

void memory_page_stop(void)
{
    lv_free(current->feature);
    current->feature = NULL;
}

void memory_page_create(lv_obj_t *screen)
{
    page_title_create(screen, "内存使用", ui_font_title());
    memory_view_t *view = lv_malloc_zeroed(sizeof(*view));
    current->feature = view;
    if (!view)
    {
        centered_label(screen, "内存不足", MARGIN, 300, CONTENT_WIDTH, ui_font_body());
        return;
    }
    for (unsigned r = 0; r < 2; ++r)
    {
        lv_obj_t *panel = panel_create(screen, MARGIN, 174 + 220 * r, CONTENT_WIDTH, 202);
        label_create(panel, r ? "RAM1" : "RAM0", 20, 14, CONTENT_WIDTH - 40, ui_font_body());
        view->usage[r] = label_create(panel, "", 20, 60, CONTENT_WIDTH - 40, ui_font_small());
        view->available[r] = label_create(panel, "", 20, 108, CONTENT_WIDTH - 40, ui_font_small());
        view->unassigned[r] = label_create(panel, "", 20, 154, CONTENT_WIDTH - 40, ui_font_caption());
    }
    button_create(screen, 252, 1140, 180, 56, "刷新", MEMORY_REFRESH);
}

bool memory_page_action(int *target)
{
    if (*target != MEMORY_REFRESH || !current->feature) return false;
    memory_page_resume();
    return true;
}
