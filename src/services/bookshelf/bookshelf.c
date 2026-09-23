/*
 * SPDX-FileCopyrightText: 2024-2025 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/**
 * @file bookshelf.c
 * @brief 书库管理服务实现：扫描 TF 卡根目录中的 TXT 书籍
 */
#include "bookshelf.h"

#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#define DBG_TAG "bookshelf"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/* 书籍扫描目录：TF 卡挂载点（根目录）。
   约定以 '/' 结尾，便于直接与文件名拼接："/" + "书名.txt" */
#define BOOKSHELF_DIR_DEFAULT "/"

static const char *g_dir = BOOKSHELF_DIR_DEFAULT;
static bookshelf_item_t g_items[BOOKSHELF_MAX_ITEMS];
static int g_count;
static rt_mutex_t g_lock;
static bookshelf_event_cb_t g_event_cb;
static rt_bool_t g_inited;

static rt_bool_t path_is_txt(const char *name)
{
    size_t len;

    if (name == RT_NULL)
        return RT_FALSE;

    len = strlen(name);
    if (len < 5)
        return RT_FALSE;

    return (name[len - 4] == '.') &&
           (name[len - 3] == 't' || name[len - 3] == 'T') &&
           (name[len - 2] == 'x' || name[len - 2] == 'X') &&
           (name[len - 1] == 't' || name[len - 1] == 'T');
}

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
    DIR *dir;
    struct dirent *ent;
    int count = 0;
    int i;

    if (!g_inited)
        return -RT_ERROR;

    rt_mutex_take(g_lock, RT_WAITING_FOREVER);

    dir = opendir(g_dir);
    if (dir != RT_NULL)
    {
        while ((ent = readdir(dir)) != RT_NULL && count < BOOKSHELF_MAX_ITEMS)
        {
            bookshelf_item_t *it;
            struct stat st;
            char full[BOOKSHELF_PATH_MAX];
            size_t nlen;

            /* 过滤：隐藏文件 / 非 .txt（目录由下面的 stat 结果排除） */
            if (ent->d_name[0] == '.' || !path_is_txt(ent->d_name))
                continue;

            rt_snprintf(full, sizeof(full), "%s%s", g_dir, ent->d_name);
            if (stat(full, &st) != 0 || !S_ISREG(st.st_mode))
                continue;

            it = &g_items[count];
            memset(it, 0, sizeof(*it));
            rt_snprintf(it->path, sizeof(it->path), "%s", full);
            /* 书名 = 文件名去掉 ".txt" 后缀（对齐初版显示纯书名） */
            nlen = strlen(ent->d_name) - 4;
            if (nlen >= sizeof(it->name))
                nlen = sizeof(it->name) - 1;
            memcpy(it->name, ent->d_name, nlen);
            it->name[nlen] = '\0';
            it->size = (uint32_t)st.st_size;
            it->progress = 0; /* TODO: 阅读进度持久化后回填 */
            count++;
        }
        closedir(dir);

        /* 按书名排序：列表很短（≤32），直接插入排序；
           对齐初版 EpubList 按 title 排序的行为 */
        for (i = 1; i < count; i++)
        {
            bookshelf_item_t tmp = g_items[i];
            int j = i - 1;

            while (j >= 0 && strcmp(g_items[j].name, tmp.name) > 0)
            {
                g_items[j + 1] = g_items[j];
                j--;
            }
            g_items[j + 1] = tmp;
        }
    }
    else
    {
        LOG_W("opendir(%s) failed (TF card mounted?)", g_dir);
    }

    g_count = count;
    rt_mutex_release(g_lock);

    LOG_I("bookshelf scan: %d book(s) in %s", count, g_dir);
    bookshelf_notify();
    return count;
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
