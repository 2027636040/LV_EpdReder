/**
 * @file bookshelf.h
 * @brief 书库管理服务 —— 对外接口（UI 书架页调用）
 *
 * 职责：扫描书库目录（默认 `/book`）中的 TXT 书籍，
 *       提供书籍列表快照（文件名 / 大小 / 阅读进度）。
 *
 * 数据流：bookshelf_refresh()（扫描，可用 msh 命令 `svc bookshelf list` 验证）
 *         → bookshelf_list() / bookshelf_get() 供 UI 渲染列表。
 *
 * 线程说明：接口线程安全；事件回调在服务线程上下文，
 *           UI 请转投 UI 线程（lv_async_call）。
 */
#ifndef __BOOKSHELF_H__
#define __BOOKSHELF_H__

#include <rtthread.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BOOKSHELF_PATH_MAX  64
#define BOOKSHELF_NAME_MAX  48
#define BOOKSHELF_MAX_ITEMS 32 /**< 单次最多枚举的书籍数量 */

/** 书籍条目 */
typedef struct
{
    char path[BOOKSHELF_PATH_MAX]; /**< 完整路径：/book/xxx.txt */
    char name[BOOKSHELF_NAME_MAX]; /**< 文件名（含扩展名） */
    uint32_t size;                 /**< 文件大小（字节） */
    int progress;                  /**< 阅读进度 0~100（未读 = 0） */
} bookshelf_item_t;

/** 列表变化回调（扫描完成后触发；服务线程上下文） */
typedef void (*bookshelf_event_cb_t)(int count);

/* ==================== 生命周期 ==================== */

/** 初始化书库服务（应用入口调用一次） */
rt_err_t bookshelf_service_init(void);

/* ==================== 数据查询 ==================== */

/**
 * @brief 重新扫描书库目录（同步，扫描期间不处理其它查询）
 * @return 发现的书籍数量（>=0）；负值为错误
 */
int bookshelf_refresh(void);

/** 当前列表中的书籍数量 */
int bookshelf_count(void);

/**
 * @brief 拷贝书籍列表（线程安全）
 * @param items     输出数组
 * @param max_items 数组容量
 * @return 实际拷贝的条数
 */
int bookshelf_list(bookshelf_item_t *items, int max_items);

/** 获取指定序号书籍（0 起始）；成功返回 true */
bool bookshelf_get(int index, bookshelf_item_t *out);

/** 书库目录（默认 "/book"） */
const char *bookshelf_dir(void);

/* ==================== 事件 ==================== */

/** 注册列表变化回调（可传 NULL 注销） */
void bookshelf_set_event_cb(bookshelf_event_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* __BOOKSHELF_H__ */
