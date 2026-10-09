#include "ui_internal.h"
#include "platform/app_catalog.h"
#include "platform/app_installer.h"
#include "platform/app_settings_registry.h"
#include "storage.h"
#include <dfs_posix.h>
#include <dlmodule.h>

#define APP_ROWS 8
#define APP_ROW_BASE (-550)
#define APP_PREVIOUS (-560)
#define APP_NEXT (-561)
#define APP_CONFIRM (-562)
#define APP_INSTALL_FLASH (-563)
#define APP_INSTALL_SD (-564)

typedef struct
{
    epd_app_entry_t entries[APP_ROWS];
    ui_page_id_t pages[APP_ROWS];
    lv_obj_t *rows[APP_ROWS], *labels[APP_ROWS], *empty, *previous, *next;
    unsigned first, count;
    bool more;
    uint32_t ticket;
    uint32_t revision;
    storage_volume_t volume;
    char selected[GUI_APP_ID_MAX_LEN];
} app_list_t;

static void list_refresh(void)
{
    app_list_t *list = current->feature;
    if (!list || app_installer_busy()) return;
    unsigned seen = 0;
    list->count = 0;
    list->more = false;
    if (current->id == UI_PAGE_APP_SETTINGS)
    {
        const app_setting_entry_t *setting;
        for (unsigned i = 0; (setting = app_settings_get(i)) != NULL; ++i)
        {
            if (seen++ < list->first) continue;
            if (list->count == APP_ROWS) { list->more = true; break; }
            unsigned row = list->count++;
            snprintf(list->entries[row].id, sizeof(list->entries[row].id), "%s", setting->id);
            snprintf(list->entries[row].name, sizeof(list->entries[row].name), "%s", setting->title);
            list->pages[row] = setting->page;
        }
    }
    storage_lock();
    bool installing = current->id == UI_PAGE_APP_INSTALL;
    for (storage_volume_t v = STORAGE_FLASH; v < (installing ? 1 : STORAGE_COUNT) && !list->more; ++v)
    {
        if (!installing && !app_installer_volume_ready(v)) continue;
        if (current->id == UI_PAGE_APP_SETTINGS && !storage_apps_enabled(v)) continue;
        const char *base = installing ? EPD_PACKAGE_DIRECTORY : storage_app_directory(v);
        DIR *dir = storage_path_available(base) ? opendir(base) : NULL;
        if (!dir) continue;
        struct dirent *file;
        epd_app_entry_t entry;
        while ((file = readdir(dir)) != NULL)
        {
            if (!app_package_read(base, file->d_name, &entry)) continue;
            if (current->id == UI_PAGE_APP_SETTINGS && !entry.has_settings) continue;
            if (current->id == UI_PAGE_APP_SETTINGS)
            {
                storage_volume_t selected;
                if (!storage_app_locate(entry.id, &selected) || selected != v) continue;
            }
            if (seen++ < list->first) continue;
            if (list->count == APP_ROWS) { list->more = true; break; }
            list->entries[list->count] = entry;
            list->pages[list->count++] = UI_PAGE_COUNT;
        }
        closedir(dir);
    }
    storage_unlock();
    for (unsigned i = 0; i < APP_ROWS; ++i)
    {
        if (i < list->count)
        {
            char name[80];
            if (current->id == UI_PAGE_APP_UNINSTALL)
                snprintf(name, sizeof(name), "%s · %s", list->entries[i].name,
                         list->entries[i].volume == STORAGE_SD ? "TF 卡" : "内部存储");
            else snprintf(name, sizeof(name), "%s", list->entries[i].name);
            text_update(list->labels[i], name);
        }
        object_visible(list->rows[i], i < list->count);
    }
    text_update(list->empty, current->id == UI_PAGE_APP_INSTALL && !storage_available(STORAGE_SD)
                ? "请插入 TF 卡" : "这里空空如也~");
    object_visible(list->empty, !list->count);
    object_visible(list->previous, list->first > 0);
    object_visible(list->next, list->more);
    list->revision = app_catalog_revision();
}

void app_list_create(lv_obj_t *screen)
{
    page_title_create(screen, titles[current->id], ui_font_title());
    app_list_t *list = lv_malloc_zeroed(sizeof(*list));
    current->feature = list;
    if (!list) { centered_label(screen, "内存不足", MARGIN, 300, CONTENT_WIDTH, ui_font_body()); return; }
    for (unsigned i = 0; i < APP_ROWS; ++i)
    {
        list->rows[i] = button_create(screen, MARGIN, 194 + i * 108, CONTENT_WIDTH, 88, NULL, APP_ROW_BASE + i);
        list->labels[i] = label_create(list->rows[i], "", 24, 24, CONTENT_WIDTH - 48, ui_font_body());
    }
    list->empty = centered_label(screen, "", MARGIN, 500, CONTENT_WIDTH, ui_font_body());
    list->previous = button_create(screen, MARGIN, 1100, 292, 76, "上一页", APP_PREVIOUS);
    list->next = button_create(screen, 360, 1100, 292, 76, "下一页", APP_NEXT);
    list_refresh();
}

void app_list_resume(void) { list_refresh(); app_list_process(); }
void app_list_stop(void) { lv_free(current->feature); current->feature = NULL; }

void app_list_storage_changed(void)
{
    app_list_t *list = current->feature;
    /* A confirmation made for a removed card must not target a replacement card. */
    if (list && !list->ticket) popup_close();
    app_list_resume();
}

void app_list_process(void)
{
    if (current->id != UI_PAGE_APP_INSTALL && current->id != UI_PAGE_APP_UNINSTALL &&
        current->id != UI_PAGE_APP_SETTINGS) return;
    app_list_t *list = current->feature;
    if (!list) return;
    if (!list->ticket)
    {
        if (!current->popup && list->revision != app_catalog_revision()) list_refresh();
        return;
    }
    app_job_result_t result;
    app_installer_result(&result);
    if (result.ticket != list->ticket || result.state == APP_JOB_RUNNING) return;
    list->ticket = 0;
    popup_close();
    list_refresh();
    popup_open(result.message, NULL);
}

static bool open_dynamic_settings(const char *id)
{
    intent_t intent = intent_init(id);
    if (!intent) return false;
    intent_set_string(intent, "page", "settings");
    int result = intent_runapp(intent);
    intent_deinit(intent);
    if (result != RT_EOK) return false;
    gui_app_exec_now();
    return gui_app_is_actived((char *)id);
}

static void select_install_volume(const char *name)
{
    popup_open("选择安装位置", name);
    lv_obj_t *dialog = lv_obj_get_parent(current->popup_button);
    lv_obj_set_y(dialog, 396);
    lv_obj_set_height(dialog, 366);
    lv_obj_set_y(current->popup_button, 268);
    text_update(lv_obj_get_child(current->popup_button, 0), "取消");
    lv_obj_t *internal = button_create(dialog, 48, 184, 222, 64, "内部存储", APP_INSTALL_FLASH);
    lv_obj_t *card = button_create(dialog, 306, 184, 222, 64, "TF 卡", APP_INSTALL_SD);
    if (!app_installer_volume_ready(STORAGE_FLASH)) lv_obj_add_state(internal, LV_STATE_DISABLED);
    if (!app_installer_volume_ready(STORAGE_SD)) lv_obj_add_state(card, LV_STATE_DISABLED);
    lv_group_focus_obj(current->popup_button);
}

bool app_list_action(int *target)
{
    app_list_t *list = current->feature;
    if (!list) return false;
    if (list->ticket) return true;
    if (*target == NAV_BACK && current->popup) { popup_close(); return true; }
    if ((*target == APP_CONFIRM || *target == APP_INSTALL_FLASH || *target == APP_INSTALL_SD) && current->popup)
    {
        bool uninstall = current->id == UI_PAGE_APP_UNINSTALL;
        if (*target != APP_CONFIRM) list->volume = *target == APP_INSTALL_SD ? STORAGE_SD : STORAGE_FLASH;
        popup_close();
        if (!app_installer_start(list->selected, uninstall, list->volume, &list->ticket))
            popup_open("操作失败，请重试", NULL);
        else
        {
            popup_open(uninstall ? "正在卸载" : "正在安装", NULL);
            object_visible(current->popup_button, false);
        }
        return true;
    }
    if (*target == APP_PREVIOUS && list->first) list->first -= APP_ROWS;
    else if (*target == APP_NEXT && list->more) list->first += APP_ROWS;
    else if (*target >= APP_ROW_BASE && *target < APP_ROW_BASE + (int)list->count)
    {
        unsigned row = *target - APP_ROW_BASE;
        if (current->id == UI_PAGE_APP_SETTINGS)
        {
            if (list->pages[row] < UI_PAGE_COUNT) { *target = list->pages[row]; return false; }
            char id[GUI_APP_ID_MAX_LEN];
            snprintf(id, sizeof(id), "%s", list->entries[row].id);
            if (!open_dynamic_settings(id) && current && current->foreground)
                popup_open("应用打开失败", NULL);
        }
        else
        {
            snprintf(list->selected, sizeof(list->selected), "%s", list->entries[row].id);
            if (current->id == UI_PAGE_APP_INSTALL)
            {
                if (storage_app_locate(list->selected, &list->volume))
                {
                    char detail[80];
                    snprintf(detail, sizeof(detail), "%s · %s", list->entries[row].name,
                             list->volume == STORAGE_SD ? "TF 卡" : "内部存储");
                    popup_confirm("更新应用？", detail, APP_CONFIRM);
                }
                else select_install_volume(list->entries[row].name);
            }
            else
            {
                list->volume = list->entries[row].volume;
                popup_confirm("卸载应用？", list->entries[row].name, APP_CONFIRM);
            }
        }
        return true;
    }
    else return false;
    list_refresh();
    return true;
}
