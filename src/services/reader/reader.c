/*
 * SPDX-FileCopyrightText: 2024-2025 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/**
 * @file reader.c
 * @brief 阅读引擎服务实现：TXT 打开 / 按偏移读取 UTF-8 文本流 / 阅读位置
 */
#include "reader.h"

#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#define DBG_TAG "reader"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/* UTF-8 BOM */
static const unsigned char UTF8_BOM[3] = { 0xEF, 0xBB, 0xBF };

typedef struct
{
    rt_bool_t opened;
    int fd;
    char path[BOOKSHELF_PATH_MAX];
    char title[BOOKSHELF_NAME_MAX];
    uint32_t data_start; /* 文本起点（跳过 BOM 后） */
    uint32_t file_size;  /* 文本总字节数（含 data_start 之前的内容） */
    char encoding[16];
    uint32_t position;
} reader_ctx_t;

static reader_ctx_t g_reader;
static rt_mutex_t g_lock;
static rt_bool_t g_inited;

/*---------------------------------------------------------------------------*/
/* UTF-8 工具 */
/*---------------------------------------------------------------------------*/
static int utf8_char_len(unsigned char c)
{
    if (c < 0x80)
        return 1;
    if ((c & 0xE0) == 0xC0)
        return 2;
    if ((c & 0xF0) == 0xE0)
        return 3;
    if ((c & 0xF8) == 0xF0)
        return 4;
    return 1; /* 非法字节按单字节处理 */
}

/**
 * @brief 裁掉尾部不完整的多字节字符，返回可安全输出的长度。
 */
static int utf8_trim_tail(const char *buf, int n)
{
    while (n > 0)
    {
        int i = n - 1;
        int clen;

        /* 回退到字符首字节 */
        while (i > 0 && (((unsigned char)buf[i] & 0xC0) == 0x80))
            i--;

        clen = utf8_char_len((unsigned char)buf[i]);
        if (i + clen <= n)
            break; /* 尾部字符完整 */

        n = i; /* 截掉不完整字符 */
    }

    return n;
}

/*---------------------------------------------------------------------------*/
/* 生命周期 */
/*---------------------------------------------------------------------------*/
rt_err_t reader_service_init(void)
{
    if (g_inited)
        return RT_EOK;

    g_lock = rt_mutex_create("rd_lock", RT_IPC_FLAG_PRIO);
    if (g_lock == RT_NULL)
        return -RT_ENOMEM;

    memset(&g_reader, 0, sizeof(g_reader));
    g_reader.fd = -1;

    g_inited = RT_TRUE;
    LOG_I("reader service started");
    return RT_EOK;
}

void reader_close(void)
{
    if (!g_inited)
        return;

    rt_mutex_take(g_lock, RT_WAITING_FOREVER);
    if (g_reader.opened && g_reader.fd >= 0)
    {
        close(g_reader.fd);
    }
    g_reader.opened = RT_FALSE;
    g_reader.fd = -1;
    rt_mutex_release(g_lock);
}

rt_err_t reader_open(const char *path)
{
    int fd;
    unsigned char bom[3];
    uint32_t start = 0;
    uint32_t size;
    off_t sz;

    if (path == RT_NULL || path[0] == '\0' || !g_inited)
        return -RT_EINVAL;

    /* 先关闭旧书 */
    reader_close();

    fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        LOG_W("open(%s) failed", path);
        return -RT_EIO;
    }

    /* 文件大小 */
    sz = lseek(fd, 0, SEEK_END);
    if (sz < 0)
    {
        close(fd);
        return -RT_EIO;
    }
    size = (uint32_t)sz;

    /* BOM 检测（当前仅处理 UTF-8；GBK/BIG5 检测与转换 TODO） */
    if (size >= 3 && read(fd, bom, 3) == 3)
    {
        if (memcmp(bom, UTF8_BOM, 3) == 0)
            start = 3;
    }

    rt_mutex_take(g_lock, RT_WAITING_FOREVER);
    memset(&g_reader, 0, sizeof(g_reader));
    g_reader.opened = RT_TRUE;
    g_reader.fd = fd;
    rt_snprintf(g_reader.path, sizeof(g_reader.path), "%s", path);
    rt_snprintf(g_reader.title, sizeof(g_reader.title), "%s", path);
    {
        /* 标题取文件名（去掉目录部分） */
        char *slash = strrchr(g_reader.title, '/');
        if (slash != RT_NULL)
            memmove(g_reader.title, slash + 1, strlen(slash));
    }
    g_reader.data_start = start;
    g_reader.file_size = size;
    g_reader.position = start;
    rt_snprintf(g_reader.encoding, sizeof(g_reader.encoding), "UTF-8");
    rt_mutex_release(g_lock);

    LOG_I("open: %s (%u bytes, encoding %s)", path, (unsigned)size, "UTF-8");
    return RT_EOK;
}

bool reader_is_open(void)
{
    bool opened;

    if (!g_inited)
        return false;

    rt_mutex_take(g_lock, RT_WAITING_FOREVER);
    opened = g_reader.opened ? true : false;
    rt_mutex_release(g_lock);
    return opened;
}

bool reader_get_info(reader_info_t *out)
{
    if (out == RT_NULL || !g_inited)
        return false;

    rt_mutex_take(g_lock, RT_WAITING_FOREVER);
    if (!g_reader.opened)
    {
        rt_mutex_release(g_lock);
        return false;
    }

    rt_snprintf(out->path, sizeof(out->path), "%s", g_reader.path);
    rt_snprintf(out->title, sizeof(out->title), "%s", g_reader.title);
    out->file_size = g_reader.file_size;
    rt_snprintf(out->encoding, sizeof(out->encoding), "%s", g_reader.encoding);
    out->position = g_reader.position;
    rt_mutex_release(g_lock);
    return true;
}

/*---------------------------------------------------------------------------*/
/* 文本读取 */
/*---------------------------------------------------------------------------*/
int reader_read_text(uint32_t offset, char *buf, rt_size_t buf_size, uint32_t *next_offset)
{
    int n;

    if (buf == RT_NULL || buf_size < 2 || !g_inited)
        return -RT_EINVAL;

    rt_mutex_take(g_lock, RT_WAITING_FOREVER);

    if (!g_reader.opened || g_reader.fd < 0)
    {
        rt_mutex_release(g_lock);
        return -RT_ERROR;
    }

    if (lseek(g_reader.fd, (off_t)offset, SEEK_SET) < 0)
    {
        rt_mutex_release(g_lock);
        return -RT_EIO;
    }

    n = read(g_reader.fd, buf, buf_size - 1);
    if (n < 0)
    {
        rt_mutex_release(g_lock);
        return -RT_EIO;
    }

    buf[n] = '\0';

    /* 尾部不完整字符截断（下次从该字符重新读） */
    n = utf8_trim_tail(buf, n);
    buf[n] = '\0';

    if (next_offset != RT_NULL)
        *next_offset = offset + (uint32_t)n;

    rt_mutex_release(g_lock);
    return n;
}

int reader_read_next(char *buf, rt_size_t buf_size)
{
    uint32_t offset;
    uint32_t next = 0;
    int n;

    if (!g_inited)
        return -RT_ERROR;

    rt_mutex_take(g_lock, RT_WAITING_FOREVER);
    offset = g_reader.position;
    rt_mutex_release(g_lock);

    n = reader_read_text(offset, buf, buf_size, &next);
    if (n >= 0)
        reader_set_position(next);

    return n;
}

/*---------------------------------------------------------------------------*/
/* 阅读位置 */
/*---------------------------------------------------------------------------*/
rt_err_t reader_set_position(uint32_t offset)
{
    if (!g_inited)
        return -RT_ERROR;

    rt_mutex_take(g_lock, RT_WAITING_FOREVER);
    if (!g_reader.opened)
    {
        rt_mutex_release(g_lock);
        return -RT_ERROR;
    }

    if (offset < g_reader.data_start)
        offset = g_reader.data_start;
    if (offset > g_reader.file_size)
        offset = g_reader.file_size;

    g_reader.position = offset;
    rt_mutex_release(g_lock);
    return RT_EOK;
}

uint32_t reader_get_position(void)
{
    uint32_t pos;

    if (!g_inited)
        return 0;

    rt_mutex_take(g_lock, RT_WAITING_FOREVER);
    pos = g_reader.position;
    rt_mutex_release(g_lock);
    return pos;
}

int reader_get_percent(void)
{
    int percent = 0;

    if (!g_inited)
        return 0;

    rt_mutex_take(g_lock, RT_WAITING_FOREVER);
    if (g_reader.opened && g_reader.file_size > g_reader.data_start)
    {
        uint32_t text_size = g_reader.file_size - g_reader.data_start;
        uint32_t pos = (g_reader.position > g_reader.data_start)
                           ? (g_reader.position - g_reader.data_start)
                           : 0;
        if (pos > text_size)
            pos = text_size;
        percent = (int)((uint64_t)pos * 100 / text_size);
    }
    rt_mutex_release(g_lock);

    return percent;
}
