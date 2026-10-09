#include "app_card.h"
#include "app_catalog.h"
#include "app_installer.h"
#include "epd_app.h"
#include "epd_input.h"
#include <dfs_posix.h>

static lv_obj_t *modal, *message;
static uint32_t prompt_session, answered_session, scanned_session, scanned_revision;
static bool requested, loading;

static void dismiss(void)
{
    if (modal) lv_obj_delete(modal);
    modal = message = NULL;
    loading = false;
    epd_input_clear();
}

static void answer(lv_event_t *event)
{
    bool accept = (uintptr_t)lv_event_get_user_data(event) != 0;
    answered_session = prompt_session;
    if (!accept || !storage_session_valid(STORAGE_SD, prompt_session)) { dismiss(); return; }
    if (!storage_card_enable_apps(prompt_session)) { dismiss(); return; }
    /* Gate execution again until migration and journal recovery have finished. */
    app_installer_storage_changed();
    app_installer_process();
    loading = true;
    epd_app_text(message, "正在加载");
    lv_obj_t *dialog = lv_obj_get_parent(message);
    for (unsigned i = 1; i < lv_obj_get_child_count(dialog); ++i)
        lv_obj_add_flag(lv_obj_get_child(dialog, i), LV_OBJ_FLAG_HIDDEN);
    epd_input_clear();
}

static bool has_apps(uint32_t session)
{
    bool found = false;
    storage_lock();
    DIR *dir = storage_session_valid(STORAGE_SD, session) ? opendir(STORAGE_SD_APPS) : NULL;
    struct dirent *file;
    epd_app_entry_t entry;
    while (dir && (file = readdir(dir)) != NULL)
        if (app_package_read(STORAGE_SD_APPS, file->d_name, &entry) && !entry.resources_only)
        { found = true; break; }
    if (dir) closedir(dir);
    storage_unlock();
    return found && storage_session_valid(STORAGE_SD, session);
}

void app_card_request(void) { requested = true; }

void app_card_process(void)
{
    if (!epd_app_refresh_done()) return;
    uint32_t session = storage_card_session();
    if (modal && !storage_session_valid(STORAGE_SD, prompt_session)) dismiss();
    if (loading)
    {
        if (app_installer_busy()) return;
        if (app_installer_volume_ready(STORAGE_SD))
        {
            app_catalog_changed();
            dismiss();
        }
        else
        {
            loading = false;
            epd_app_text(message, "加载失败，请重试");
            lv_obj_t *dialog = lv_obj_get_parent(message);
            for (unsigned i = 1; i < lv_obj_get_child_count(dialog); ++i)
                lv_obj_remove_flag(lv_obj_get_child(dialog, i), LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    if (modal || storage_changing() || app_installer_busy()) return;
    if (!storage_available(STORAGE_SD)) { requested = false; return; }
    if (!requested && (answered_session == session || storage_apps_enabled(STORAGE_SD))) return;
    uint32_t revision = app_catalog_revision();
    if (!requested && scanned_session == session && scanned_revision == revision) return;
    scanned_session = session;
    scanned_revision = revision;
    bool found = has_apps(session);
    if (!storage_session_valid(STORAGE_SD, session)) return;
    bool enabled = storage_apps_enabled(STORAGE_SD) && app_installer_volume_ready(STORAGE_SD);
    if (!found && !requested) return;
    requested = false;
    prompt_session = session;
    /* System overlay survives app_fwk navigation and owns its input scope. */
    modal = epd_app_panel(lv_layer_top(), 0, 0, 684, 1216);
    lv_obj_set_style_bg_opa(modal, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_add_flag(modal, LV_OBJ_FLAG_CLICKABLE);
    epd_app_input_group(modal);
    lv_obj_t *dialog = epd_app_panel(modal, 54, 438, 576, 282);
    lv_obj_set_style_border_color(dialog, lv_color_black(), 0);
    lv_obj_set_style_border_width(dialog, 2, 0);
    message = epd_app_centered_label(dialog, enabled ? "TF 卡应用已加载" :
                                    found ? "是否加载 TF 卡内应用？" : "TF 卡内没有已安装应用",
                                    24, 74, 528, epd_app_font());
    lv_obj_t *cancel = epd_app_button(dialog, 48, 184, 222, 64, "取消", answer, NULL);
    lv_obj_t *confirm = epd_app_button(dialog, 306, 184, 222, 64, "加载", answer, (void *)1);
    if (!found || enabled) lv_obj_add_state(confirm, LV_STATE_DISABLED);
    epd_app_input_back(modal, cancel);
    lv_group_focus_obj(cancel);
    epd_input_clear();
}
