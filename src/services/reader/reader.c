/*
 * SPDX-FileCopyrightText: 2024-2025 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/**
 * @file reader.c
 * @brief 阅读引擎服务实现：TXT 打开 / 按偏移读取文本流（UTF-8·GBK 自动转码）/ 阅读位置
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

/* GBK→Unicode：复用 FatFs 936 码表（ffunicode.c 已随 elmfat 编入固件，零额外 Flash；
   原型见组件头 ff.h:368，输入为 (首字节<<8)|次字节；码页须与 CONFIG_RT_DFS_ELM_CODE_PAGE 一致） */
extern unsigned short ff_oem2uni(unsigned short oem, unsigned short cp);
#define READER_GBK_CODE_PAGE 936

/* GBK 转码原始字节暂存（先读原文再逐字符展开，避免就地展开覆盖未读字节） */
static unsigned char g_gbk_raw[1024];

typedef struct
{
    rt_bool_t opened;
    int fd;
    char path[BOOKSHELF_PATH_MAX];
    char title[BOOKSHELF_NAME_MAX];
    uint32_t data_start; /* 文本起点（跳过 BOM 后） */
    uint32_t file_size;  /* 文本总字节数（含 data_start 之前的内容） */
    char encoding[16];
    rt_bool_t gbk;       /* true = GBK 源文件（读取时转 UTF-8 输出） */
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
/* GBK → UTF-8（复用 FatFs 936 码表）                                          */
/*---------------------------------------------------------------------------*/
static rt_bool_t is_gbk_lead(unsigned char c)
{
    return (c >= 0x81 && c <= 0xFE) ? RT_TRUE : RT_FALSE;
}

/* Unicode(BMP) → UTF-8，返回写出字节数 */
static int utf8_emit(unsigned short uni, char *out)
{
    if (uni < 0x80)
    {
        out[0] = (char)uni;
        return 1;
    }
    if (uni < 0x800)
    {
        out[0] = (char)(0xC0 | (uni >> 6));
        out[1] = (char)(0x80 | (uni & 0x3F));
        return 2;
    }
    out[0] = (char)(0xE0 | (uni >> 12));
    out[1] = (char)(0x80 | ((uni >> 6) & 0x3F));
    out[2] = (char)(0x80 | (uni & 0x3F));
    return 3;
}

/* 采样校验 UTF-8：出现非法序列即认定为非 UTF-8（样本尾部截断忽略） */
static rt_bool_t utf8_valid(const unsigned char *p, int n)
{
    int i = 0;

    while (i < n)
    {
        unsigned char c = p[i];
        int len;
        int k;

        if (c < 0x80)
        {
            i++;
            continue;
        }

        if ((c & 0xE0) == 0xC0)
            len = 2;
        else if ((c & 0xF0) == 0xE0)
            len = 3;
        else if ((c & 0xF8) == 0xF0)
            len = 4;
        else
            return RT_FALSE; /* 孤立续字节或非法首字节 */

        if (c < 0xC2 || c > 0xF4) /* 拒绝 C0/C1 超长编码与 F5+ */
            return RT_FALSE;

        if (i + len > n)
            break; /* 样本被截断，忽略尾部 */

        for (k = 1; k < len; k++)
        {
            if ((p[i + k] & 0xC0) != 0x80)
                return RT_FALSE;
        }
        i += len;
    }

    return RT_TRUE;
}

/* 编码检测：UTF-8 合法（或纯 ASCII）→ UTF-8；否则按 GBK */
static rt_bool_t detect_gbk(int fd, uint32_t size)
{
    static unsigned char sample[2048];
    rt_size_t want = (size > sizeof(sample)) ? sizeof(sample) : size;
    int n;

    if (lseek(fd, 0, SEEK_SET) < 0)
        return RT_FALSE;

    n = read(fd, sample, want);
    if (n <= 0)
        return RT_FALSE;

    return utf8_valid(sample, n) ? RT_FALSE : RT_TRUE;
}

/* GBK 读取并转 UTF-8（调用方需已持有读锁）：
   最多消费 (buf_size-1)*2/3 个原始字节，保证输出不超缓冲 */
static int reader_read_gbk(int fd, uint32_t offset, char *buf, rt_size_t buf_size, uint32_t *next_offset)
{
    rt_size_t max_raw = (buf_size - 1) * 2 / 3;
    int n;
    int out = 0;
    int i;

    if (max_raw > sizeof(g_gbk_raw))
        max_raw = sizeof(g_gbk_raw);

    if (lseek(fd, (off_t)offset, SEEK_SET) < 0)
        return -RT_EIO;

    n = read(fd, g_gbk_raw, max_raw);
    if (n < 0)
        return -RT_EIO;

    /* 尾部只剩 GBK 首字节 → 不消费，留给下次（偏移始终落在字符边界） */
    if (n > 0 && is_gbk_lead(g_gbk_raw[n - 1]))
        n--;

    for (i = 0; i < n;)
    {
        unsigned char c = g_gbk_raw[i];

        if (c < 0x80) /* ASCII 直通 */
        {
            buf[out++] = (char)c;
            i++;
            continue;
        }

        if (is_gbk_lead(c) && (i + 1) < n &&
            g_gbk_raw[i + 1] >= 0x40 && g_gbk_raw[i + 1] != 0x7F)
        {
            unsigned short uni = ff_oem2uni((unsigned short)((c << 8) | g_gbk_raw[i + 1]),
                                            READER_GBK_CODE_PAGE);
            if (uni != 0)
                out += utf8_emit(uni, &buf[out]);
            else
                buf[out++] = '?'; /* 码表未定义（含 GB18030 四字节序列） */
            i += 2;
            continue;
        }

        buf[out++] = '?'; /* 非法字节 */
        i++;
    }

    buf[out] = '\0';
    if (next_offset != RT_NULL)
        *next_offset = offset + (uint32_t)n;

    return out;
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
    rt_bool_t gbk;

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

    /* BOM 检测（UTF-8 BOM 跳过） */
    if (size >= 3 && read(fd, bom, 3) == 3)
    {
        if (memcmp(bom, UTF8_BOM, 3) == 0)
            start = 3;
    }

    /* 编码检测：带 BOM → UTF-8；否则采样校验，不合法按 GBK（读取时转 UTF-8） */
    gbk = (start == 0) ? detect_gbk(fd, size) : RT_FALSE;

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
    g_reader.gbk = gbk;
    rt_snprintf(g_reader.encoding, sizeof(g_reader.encoding), gbk ? "GBK" : "UTF-8");
    rt_mutex_release(g_lock);

    LOG_I("open: %s (%u bytes, encoding %s)", path, (unsigned)size, gbk ? "GBK" : "UTF-8");
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

    if (g_reader.gbk)
    {
        n = reader_read_gbk(g_reader.fd, offset, buf, buf_size, next_offset);
        rt_mutex_release(g_lock);
        return n;
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
