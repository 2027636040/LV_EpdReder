#include "ui_internal.h"
#include "storage.h"
#include <dfs_posix.h>
#include <time.h>

#define FILE_ROWS 8
#define FILE_ROW_BASE (-520)
#define FILE_PREVIOUS (-530)
#define FILE_NEXT (-531)

typedef struct
{
    char path[STORAGE_PATH_MAX];
    char names[FILE_ROWS][256];
    bool directories[FILE_ROWS];
    unsigned first, count;
    bool more;
    lv_obj_t *rows[FILE_ROWS], *labels[FILE_ROWS], *details[FILE_ROWS];
    lv_obj_t *breadcrumb, *empty, *previous, *next;
} files_view_t;

static void files_render(void)
{
    files_view_t *view = current->feature;
    if (!view) return;
    view->count = 0;
    view->more = false;
    text_update(view->empty, "这里空空如也~");
    if (!view->path[0])
    {
        text_update(view->breadcrumb, "");
        for (unsigned i = 0; i < STORAGE_COUNT; ++i)
        {
            storage_info_t info;
            char text[96], total[24], used[24];
            if (storage_info(i, &info))
            {
                storage_format_size(info.total, total, sizeof(total));
                storage_format_size(info.used, used, sizeof(used));
                snprintf(text, sizeof(text), "已用 %s / 共 %s", used, total);
            }
            else snprintf(text, sizeof(text), "%s", i == STORAGE_SD ? "未插入或未挂载" : "未挂载");
            text_update(view->labels[i], i == STORAGE_SD ? "TF 卡" : "内部存储空间");
            text_update(view->details[i], text);
        }
        view->count = STORAGE_COUNT;
    }
    else
    {
        text_update(view->breadcrumb, view->path);
        storage_lock();
        storage_volume_t volume = !strcmp(storage_path_root(view->path), STORAGE_SD_ROOT) ? STORAGE_SD : STORAGE_FLASH;
        DIR *dir = storage_available(volume) ? opendir(view->path) : NULL;
        if (dir)
        {
            struct dirent *entry;
            unsigned skipped = 0;
            while ((entry = readdir(dir)) != NULL)
            {
                if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
                if (skipped++ < view->first) continue;
                if (view->count == FILE_ROWS) { view->more = true; break; }
                unsigned row = view->count++;
                snprintf(view->names[row], sizeof(view->names[row]), "%s", entry->d_name);
                char path[STORAGE_PATH_MAX], size[32];
                struct stat st;
                int n = snprintf(path, sizeof(path), "%s/%s", view->path, entry->d_name);
                bool valid = n > 0 && n < sizeof(path) && stat(path, &st) == 0;
                view->directories[row] = valid && S_ISDIR(st.st_mode);
                if (view->directories[row]) strcpy(size, "文件夹  ›");
                else if (valid) storage_format_size(st.st_size, size, sizeof(size));
                else strcpy(size, "无法读取");
                text_update(view->labels[row], entry->d_name);
                text_update(view->details[row], size);
            }
            closedir(dir);
        }
        else text_update(view->empty, "存储不可用");
        storage_unlock();
    }
    for (unsigned i = 0; i < FILE_ROWS; ++i) object_visible(view->rows[i], i < view->count);
    object_visible(view->empty, !view->count);
    object_visible(view->previous, view->path[0] && view->first);
    object_visible(view->next, view->more);
}

void files_resume(void) { files_render(); }
void files_storage_changed(void)
{
    files_view_t *view = current->feature;
    if (view && view->path[0] && !strcmp(storage_path_root(view->path), STORAGE_SD_ROOT))
    {
        snprintf(view->path, sizeof(view->path), "%s", STORAGE_SD_ROOT);
        view->first = 0;
        if (current->popup) popup_close();
    }
    files_render();
}
void storage_page_resume(void) { files_render(); }
void files_stop(void) { lv_free(current->feature); current->feature = NULL; }

void files_create(lv_obj_t *screen)
{
    page_title_create(screen, titles[current->id], ui_font_title());
    files_view_t *view = lv_malloc_zeroed(sizeof(*view));
    current->feature = view;
    if (!view) { centered_label(screen, "内存不足", MARGIN, 300, CONTENT_WIDTH, ui_font_body()); return; }
    view->breadcrumb = label_create(screen, "", MARGIN, 174, CONTENT_WIDTH, ui_font_small());
    for (unsigned i = 0; i < FILE_ROWS; ++i)
    {
        view->rows[i] = button_create(screen, MARGIN, 228 + i * 104, CONTENT_WIDTH, 94, NULL, FILE_ROW_BASE + i);
        view->labels[i] = label_create(view->rows[i], "", 20, 10, CONTENT_WIDTH - 40, ui_font_body());
        view->details[i] = label_create(view->rows[i], "", 20, 51, CONTENT_WIDTH - 40, ui_font_caption());
    }
    view->empty = centered_label(screen, "", MARGIN, 514, CONTENT_WIDTH, ui_font_body());
    view->previous = button_create(screen, MARGIN, 1100, 292, 76, "上一页", FILE_PREVIOUS);
    view->next = button_create(screen, 360, 1100, 292, 76, "下一页", FILE_NEXT);
    files_render();
}
void storage_page_create(lv_obj_t *screen) { files_create(screen); }

bool files_action(int *target)
{
    files_view_t *view = current->feature;
    if (!view) return false;
    if (*target == NAV_BACK && current->popup) { popup_close(); return true; }
    if (*target == NAV_BACK && view->path[0])
    {
        char *slash = strrchr(view->path, '/');
        if (slash) *slash = 0;
        view->first = 0;
    }
    else if (*target == FILE_PREVIOUS && view->first) view->first -= FILE_ROWS;
    else if (*target == FILE_NEXT && view->more) view->first += FILE_ROWS;
    else if (*target >= FILE_ROW_BASE && *target < FILE_ROW_BASE + (int)view->count)
    {
        unsigned i = *target - FILE_ROW_BASE;
        if (!view->path[0])
        {
            if (!storage_available(i)) { popup_open("存储不可用", NULL); return true; }
            snprintf(view->path, sizeof(view->path), "%s", storage_root(i));
        }
        else if (view->directories[i])
        {
            size_t n = strlen(view->path);
            if (n + strlen(view->names[i]) + 2 > sizeof(view->path))
            { popup_open("路径过长", NULL); return true; }
            snprintf(view->path + n, sizeof(view->path) - n, "/%s", view->names[i]);
        }
        else
        {
            popup_open(view->names[i], lv_label_get_text(view->details[i]));
            return true;
        }
        view->first = 0;
    }
    else return false;
    files_render();
    focus_restore(0);
    return true;
}

static int app_main(intent_t intent)
{
    (void)intent;
    return ui_navigation_start("files", UI_PAGE_FILES);
}
BUILTIN_APP_EXPORT("文件管理", &ui_icon_transfer, "files", app_main, 1);
