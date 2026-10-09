# 数据服务层（services）

> 本层由「设备侧」维护，向上（`src/ui/`）提供**纯数据接口**。
> UI 同事只需 `#include` 对应头文件并调用接口，无需关心蓝牙 / 网络 / HTTP 细节。

## 模块一览

| 模块 | 头文件 | 职责 |
|---|---|---|
| 蓝牙 PAN 联网 | `services/net/bt_pan.h` | 手机蓝牙配对/连接、PAN 网络共享（互联网出口） |
| 统一网络 | `services/net/network.h` | 路由与 DNS 选择、网络快照、多使用者事件订阅 |
| 后台任务 | `services/app_service.h` | 常驻线程或 UI 分步任务、停止协议、动态模块引用保持 |
| 阅读应用私有服务 | `modules/books/bookshelf.h`、`reader.h` | 扫描双卷根目录 TXT、UTF-8/GBK 解码和阅读位置；随 `books.so` 编译，不属于公共服务 |

## 联网校时

所选网络就绪后，独立的 net_time 线程接收网络事件，再通过
NETUTILS_NTP_HOSTNAME 指定的服务器校时（默认 ntp.aliyun.com）。成功后一次性写入 RTC，
系统 time() 和界面状态栏随之使用新时间；当前时区为北京时间（UTC+8）。
同一次连接成功后不重复校时，断开并重新联网后再次同步。

NTP 响应等待上限为 5 秒，失败间隔 30 秒重试，每次连接最多请求 3 次。
断线或切换网络后放弃旧连接的结果，失败时保留原 RTC 时间。校时仅输出 net.time 串口日志，
不弹窗、不直接操作 LVGL，也不阻塞蓝牙回调或天气线程。

## 天气应用边界

天气 UI、业务线程及私有数据接口位于 `modules/weather/`，随 `weather.so` 安装，
不再编译进公共服务层。请求、持久化、接口字段和 TF 配置格式见
[天气应用说明](../../modules/weather/README.md)。

平台通过 `app_service` 管理任务寿命及网络订阅。天气线程发布主页摘要副本，
UI 从平台读取，不调用已卸载模块的函数。天气卸载不影响 PAN、NTP 或其他网络使用者。

## 设置页“蓝牙”开关 / 状态栏图标

```c
#include "bt_pan.h"
#include "network.h"

/* 开关 */
btpan_enable(true_or_false);

/* 蓝牙状态栏读取设备状态；网络业务使用 network.h，而不是蓝牙状态。 */
btpan_state_t st = btpan_get_state();
bool net_ok = network_ready();
```

## 阅读应用私有接口

以下接口只在 `modules/books/` 内使用；主固件不调用阅读模块函数。
平台通过受管 `ui_process` 保持分页寿命，模块停止后才能释放文件、字体、互斥锁和装载区。


```c
#include "bookshelf.h"
#include "reader.h"

/* 书架：扫描并取列表 */
int n = bookshelf_refresh();
bookshelf_item_t item;
bookshelf_get(i, &item);            /* item.name / item.size / item.progress */

/* 阅读：打开并分页取文本（UI 按 LVGL 度量分页） */
reader_open(item.path);
char buf[256];
uint32_t next;
int len = reader_read_text(offset, buf, sizeof(buf), &next);
/* 渲染 buf（UTF-8），下一页用 offset = next */
reader_set_position(offset);        /* 记录进度 */
```

## 联调说明

内部存储和 TF 卡分别挂载到 `/flash`、`/sdcard`。`storage.c` 负责双卷状态和 TF 热插拔；
拔卡先等待 UI 释放持久文件与字体，再等待短时 I/O 完成，最后卸载并通知 SDK 移除设备。
前台通过存储版本号更新文件列表与容量，后台线程不直接访问 LVGL 控件。

- 手机端操作：打开手机蓝牙 → 搜索设备 `RT-EPD-Reader` → 配对连接 →
  在手机侧开启“蓝牙网络共享 / 个人热点（蓝牙）”。
- 设备侧自动流程：配对/加密完成 → 3 秒后自动发起 PAN 连接；
  后续蓝牙重连由 PAN 服务管理，天气只等待统一网络就绪，不直接控制蓝牙。
- 书库：将 `.txt` 书籍放在 **TF 卡或内部存储根目录**（TF 卡需 FAT32；只扫描根目录一层，
  不含子文件夹）。已安装且运行的 `books.so` 提供 `svc bookshelf list`；
  列表按书名排序，`item.name` 为去掉 `.txt` 后缀的书名。
- 串口调试命令（msh）：

```
svc                          # 列出所有服务与子命令
svc weather status           # 已安装且运行的天气模块提供以下天气命令
svc weather refresh          # 异步请求刷新，用 status 查看完成情况
svc weather city [id]        # 查看/设置城市
svc pan status|on|off|connect
svc bookshelf list           # 扫描并打印书库
svc reader open <path>       # 打开书籍
svc reader info              # 书籍信息 + 当前位置
svc reader read [offset] [len]  # 按偏移读取一段文本
svc reader next [len]        # 从当前位置读取并前进
svc reader seek <offset>     # 设置阅读位置
```

## 待办（后续迭代）

- [x] 天气缓存、TF 导入的配置和校验后的城市持久化到应用安装卷
- [x] 空气质量（cn-mee AQI；不可用时显示 --）
- [ ] reader：BIG5 编码转换（GBK 已支持自动检测并转 UTF-8）
- [x] 阅读 UI：通过 `modules/books/ui_bookshelf_data.c` 在应用安装卷的 `data/books/` 保存原文件偏移、页码和进度；
  `reader_set_position()` 本身只更新服务内存位置。
