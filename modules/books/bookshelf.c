/*
 * SPDX-FileCopyrightText: 2024-2025 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/**
 * @file bookshelf.c
 * @brief 扫描内部存储和 TF 卡中的电子书文件与分卷 EPUB
 */
#include "bookshelf.h"
#include "storage.h"
#include "storage_file.h"
#include "document/document.h"

#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#define DBG_TAG "bookshelf"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/* Independent volumes keep their own book files and reading records. */
#define BOOKSHELF_DIR_DEFAULT STORAGE_SD_ROOT ", " STORAGE_FLASH_ROOT

static const char *g_dir = BOOKSHELF_DIR_DEFAULT;
static bookshelf_item_t g_items[BOOKSHELF_MAX_ITEMS];
static int g_count;
static rt_mutex_t g_lock;
static bookshelf_event_cb_t g_event_cb;
static rt_bool_t g_inited;

static void bookshelf_notify(void)
{
    if (g_event_cb != RT_NULL)
        g_event_cb(g_count);
}

rt_err_t bookshelf_service_init(void)
{
    if (g_inited)
        return RT_EOK;

    g_lock = rt_mutex_create("bs_lock", RT_IPC_FLAG_PRIO);
    if (g_lock == RT_NULL)
        return -RT_ENOMEM;

    g_count = 0;
    g_inited = RT_TRUE;
    LOG_I("bookshelf service started, dir=%s", g_dir);
    return RT_EOK;
}

int bookshelf_refresh(void)
{
    if (!g_inited) return -RT_ERROR;
    bool available = false;
    int count = 0;
    storage_lock();
    rt_mutex_take(g_lock, RT_WAITING_FOREVER);
    for (unsigned volume = 0; volume < STORAGE_COUNT; ++volume)
    {
        if (!storage_available(volume)) continue;
        const char *base = storage_root(volume);
        DIR *dir = opendir(base);
        if (!dir) continue;
        available = true;
        struct dirent *entry;
        while (count < BOOKSHELF_MAX_ITEMS && (entry = readdir(dir)) != NULL)
        {
            if (entry->d_name[0] == '.') continue;
            char full[BOOKSHELF_PATH_MAX];
            struct stat st;
            if (strlen(base) + 1 + strlen(entry->d_name) >= sizeof(full)) continue;
            rt_snprintf(full, sizeof(full), "%s/%s", base, entry->d_name);
            if (stat(full, &st) != 0) continue;
            document_format_t format = document_format(full, S_ISDIR(st.st_mode));
            if (format == DOCUMENT_UNKNOWN || (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))) continue;
            uint32_t modified = (uint32_t)st.st_mtime;
            uint32_t size = (uint32_t)st.st_size;
            if (S_ISDIR(st.st_mode))
            {
                if (!document_split_identity(full, &size, &modified)) continue;
            }
            else if (volume == STORAGE_FLASH && !storage_file_signature(full, &modified)) continue;
            bookshelf_item_t *item = &g_items[count++];
            memset(item, 0, sizeof(*item));
            rt_snprintf(item->path, sizeof(item->path), "%s", full);
            const char *dot = S_ISDIR(st.st_mode) ? NULL : strrchr(entry->d_name, '.');
            size_t length = dot ? (size_t)(dot - entry->d_name) : strlen(entry->d_name);
            if (length >= sizeof(item->name))
            {
                length = sizeof(item->name) - 1;
                while (length && (((unsigned char)entry->d_name[length] & 0xC0) == 0x80)) --length;
            }
            memcpy(item->name, entry->d_name, length);
            item->name[length] = '\0';
            item->size = size;
            item->modified = modified;
            item->format = format;
        }
        closedir(dir);
    }
    for (int i = 1; i < count; ++i)
    {
        bookshelf_item_t item = g_items[i];
        int j = i - 1;
        while (j >= 0 && strcmp(g_items[j].name, item.name) > 0)
        {
            g_items[j + 1] = g_items[j];
            --j;
        }
        g_items[j + 1] = item;
    }
    g_count = count;
    rt_mutex_release(g_lock);
    storage_unlock();
    LOG_I("bookshelf scan: %d book(s) in %s", count, g_dir);
    bookshelf_notify();
    return available ? count : -RT_EIO;
}

int bookshelf_count(void)
{
    int count;

    if (!g_inited)
        return 0;

    rt_mutex_take(g_lock, RT_WAITING_FOREVER);
    count = g_count;
    rt_mutex_release(g_lock);
    return count;
}

int bookshelf_list(bookshelf_item_t *items, int max_items)
{
    int count;

    if (items == RT_NULL || max_items <= 0 || !g_inited)
        return 0;

    rt_mutex_take(g_lock, RT_WAITING_FOREVER);
    count = (g_count < max_items) ? g_count : max_items;
    if (count > 0)
        memcpy(items, g_items, (rt_size_t)count * sizeof(bookshelf_item_t));
    rt_mutex_release(g_lock);

    return count;
}

bool bookshelf_get(int index, bookshelf_item_t *out)
{
    bool ret = false;

    if (out == RT_NULL || !g_inited)
        return false;

    rt_mutex_take(g_lock, RT_WAITING_FOREVER);
    if (index >= 0 && index < g_count)
    {
        *out = g_items[index];
        ret = true;
    }
    rt_mutex_release(g_lock);

    return ret;
}

const char *bookshelf_dir(void)
{
    return g_dir;
}

void bookshelf_set_event_cb(bookshelf_event_cb_t cb)
{
    g_event_cb = cb;
}

void bookshelf_service_deinit(void)
{
    if (!g_inited) return;
    g_event_cb = NULL;
    g_count = 0;
    rt_mutex_delete(g_lock);
    g_lock = NULL;
    g_inited = RT_FALSE;
}
