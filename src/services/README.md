# 数据服务层（services）

> 本层由「设备侧」维护，向上（`src/ui/`）提供**纯数据接口**。
> UI 同事只需 `#include` 对应头文件并调用接口，无需关心蓝牙 / 网络 / HTTP 细节。

## 模块一览

| 模块 | 头文件 | 职责 |
|---|---|---|
| 蓝牙 PAN 联网 | `services/net/bt_pan.h` | 手机蓝牙配对/连接、PAN 网络共享（互联网出口） |
| 天气数据 | `services/weather/weather.h` | PAN 直连和风、定时更新、任务结果、互斥快照及 NOR 持久化 |
| 书库管理 | `services/bookshelf/bookshelf.h` | 扫描 TF 卡根目录 TXT 书籍列表（书名/大小/阅读进度） |
| 阅读引擎 | `services/reader/reader.h` | 打开书籍（自动识别 UTF-8/GBK 并统一转 UTF-8 输出）、按“字节偏移”取文本、读写阅读位置 |

## PAN 联网校时

每次 PAN 连接后，独立的 pan_time 线程等待蓝牙网卡取得 IP 和网关，再通过
NETUTILS_NTP_HOSTNAME 指定的服务器校时（默认 ntp.aliyun.com）。成功后一次性写入 RTC，
系统 time() 和界面状态栏随之使用新时间；当前时区为北京时间（UTC+8）。
同一次连接成功后不重复校时，断开并重新联网后再次同步。

NTP 响应等待上限为 5 秒，失败间隔 30 秒重试，每次连接最多请求 3 次。
断线后放弃旧连接的结果，失败时保留原 RTC 时间。校时仅输出 pan.time 串口日志，
不弹窗、不直接操作 LVGL，也不阻塞蓝牙回调或天气线程。

## 天气数据与线程

设备通过手机蓝牙 PAN 直接连接和风 HTTPS 服务，不经过上位机。PAN Profile 连接后，
天气线程还会在 lwIP 线程中检查蓝牙网卡的 Link、IP 和网关；自动同步保留待处理状态，
直到网卡就绪后才开始请求。手动同步和城市校验允许等待网络，轮询等待时间最多约 15 秒。
DNS 使用 lwIP 的线程安全解析接口。TLS 连接、握手及响应读取采用非阻塞套接字和 30 秒截止时间，
保留 SDK 的 CA 链及域名校验；不跟随重定向，不在 URL 或日志中输出 API Key。
TLS 的证书有效期检查仍取决于 SDK 的 MBEDTLS_HAVE_TIME_DATE 配置。

单个 weather 线程顺序执行网络请求、城市校验、TF 配置导入和 NOR 写入。
UI 使用 weather_get_info() 复制由互斥锁保护的完整快照；锁内不执行网络或 Flash 操作。
发布数据时增加 revision，ui_weather_data.c 在 UI 线程转换显示文字，只有值发生变化的控件才重绘。
页面创建、定时读取和手动同步完成均通过同一入口发布主页摘要，天气页读取同一版本的数据。
后台更新不修改前台任务的 ticket 或结果，因此不会错误地关闭用户正在等待的弹窗。

### 请求与结果

```c
uint32_t ticket;
weather_request_job(WEATHER_JOB_SYNC, NULL, &ticket);
/* WEATHER_JOB_IMPORT: 手动导入 TF 配置；WEATHER_JOB_CITY: 传数字城市 ID。 */

weather_result_t result;
weather_get_result(&result);
if (result.ticket == ticket && !result.busy) {
    /* result.success / result.message；只能在 UI 线程操作 LVGL。 */
}

weather_info_t snapshot;
weather_get_info(&snapshot); /* 返回值及 snapshot.valid 表示是否有缓存。 */
```

每次只受理一个前台任务。后台请求串行执行，不弹窗；前台请求排队时不覆盖正在使用的配置。
开机先恢复本地缓存，配置和城市有效且 PAN 网卡取得 IP 和网关后同步一次，
断线重连后补发，并保留固定 30 分钟周期；离线时到期的同步任务不会被丢弃。
天气服务使用 PAN 状态回调记录重连代次；回调只记录事件，网络请求仍在 weather 线程执行。
导入配置或成功设置城市后安排静默同步。同步失败时保留上一次完整快照和待同步状态，
从失败结束开始等待 5 秒再重试；离线期间不发送请求，网络恢复后继续。
单个 HTTP 请求保留最多两次尝试，失败后的尝试间隔为 5 秒。
手动同步仍可主动发起；手动同步成功会清除待重试任务，失败则安排后台静默重试。
进入或离开天气页不会触发额外 HTTP 请求。

手动同步显示“正在同步”，失败显示“同步失败，请重试”，成功显示“同步成功”。
成功消息完成墨水屏刷新后保留一秒再关闭。等待弹窗无动画，数字输入无闪烁光标，
后台请求不触发全屏重建。

### 接口与字段

| 请求 | 页面内容 |
|---|---|
| /geo/v2/city/lookup?location=ID | 精确核对返回 ID，获得城市名、区域和经纬度 |
| /v7/weather/now?location=ID | 当前温度、体感、天气代码、风向风级、湿度、风速、能见度、气压、云量 |
| /v7/weather/7d?location=ID | 今日高低温、日出日落，以及跳过今天后的三天预报 |
| /airquality/v1/current/纬度/经度 | 中国 cn-mee AQI 及返回的等级文字 |

使用专属 API Host 和 X-QW-Api-Key 请求头，lang=zh。HTTP/JSON 错误不会发布为有效天气。
实况和三天预报以及本地保存均成功才报告同步成功；空气质量为可选数据，失败时显示 --，
不会把其他国家的指数或 QAQI 当作中国 AQI。缺失数值用 WEATHER_MISSING，AQI 缺失用 -1，
风级区间保留原始字符串，预报日期使用接口返回日期，不写死星期。
响应体及 gzip 解压各限制为 32 KiB，gzip 校验长度与 CRC；解压器放在堆上，避免占用大块线程栈。
网络失败或服务器 5xx 最多再试一次，4xx 不盲目重试。账户需具备相应接口权限。

实况和预报采用城市天气 v7 数据模型，字段遵循下方链接的 v7 文档。官方已标注 v7 即将弃用。

### TF 配置与持久化

TF 卡根目录放置 UTF-8 的 qweather.json，固定两个字符串字段：

```json
{
  "api_key": "YOUR_API_KEY",
  "api_host": "YOUR_API_HOST.qweatherapi.com"
}
```

设置 → 天气设置 → 从 TF 卡导入天气配置。只读取真实 SD 设备的挂载点，不把 NOR 同名文件
当成 TF 配置；SD 尚未挂载时尝试挂到 /sdcard，不自动格式化存储设备。
导入不立即验证密钥，城市校验或天气同步时由服务器验证。保存失败不替换现有配置。

城市设置先显示 LVGL 二维码，地址为和风官方城市列表页；点击“我已获取城市ID”后进入数字键盘。
输入 ID 必须与 GeoAPI 返回的 ID 一致，网络校验和保存成功才改变城市，并清除其他城市的缓存。
配置和天气一同保存到 weather_store 独立 NOR 区域（0x121B0000，64 KiB），
采用 16 个 4 KiB 槽轮换、版本/序号/CRC 校验，最后写提交标记。重启选择最新有效槽。
该区域不属于 TF 文件系统，移除 TF 卡仍可使用已导入的配置和缓存。

参考：[请求配置](https://dev.qweather.com/docs/configuration/api-config/)、
[城市查询](https://dev.qweather.com/docs/api/geoapi/city-lookup/)、
[城市实况](https://dev.qweather.com/docs/api/weather/weather-now-webapi-v7/)、
[城市预报](https://dev.qweather.com/docs/api/weather/weather-daily-forecast-webapi-v7/)、
[空气质量](https://dev.qweather.com/docs/api/air-quality/air-current/)。

### 3. 设置页“蓝牙”开关 / 状态栏图标

```c
#include "bt_pan.h"

/* 开关 */
btpan_enable(true_or_false);

/* 状态栏：注册状态变化（单回调槽，UI 内部可再分发） */
void on_bt_state_changed(btpan_state_t state)
{
    /* 服务线程上下文：lv_async_call 切回 UI 线程 */
}

/* 或者轮询（lv_timer 里读状态即可） */
btpan_state_t st = btpan_get_state();
bool net_ok = btpan_is_network_ready();
```

### 4. 书架页 / 阅读页

```c
#include "bookshelf.h"
#include "services/reader/reader.h"

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

- 手机端操作：打开手机蓝牙 → 搜索设备 `RT-EPD-Reader` → 配对连接 →
  在手机侧开启“蓝牙网络共享 / 个人热点（蓝牙）”。
- 设备侧自动流程：配对/加密完成 → 3 秒后自动发起 PAN 连接；
  天气请求时会再次确保网络（未连时自动请求连接并等待）。
- 书库：把 `.txt` 书籍放在 **TF 卡根目录**（卡需 FAT32 格式；只扫描根目录一层，
  不含子文件夹），`ls` 能看到的 `.txt` 即会被 `svc bookshelf list` 列出；
  列表按书名排序，`item.name` 为去掉 `.txt` 后缀的书名。
- 串口调试命令（msh）：

```
svc                          # 列出所有服务与子命令
svc weather status           # 打印天气快照（全部字段）
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

- [x] 天气缓存、TF 导入的配置和校验后的城市持久化到独立 NOR 区域
- [x] 空气质量（cn-mee AQI；不可用时显示 --）
- [ ] reader：BIG5 编码转换（GBK 已支持自动检测并转 UTF-8）
- [x] 阅读 UI：通过 `ui_bookshelf_data.c` 在 `/.epd_reader/` 保存原文件偏移、页码和进度；
  `reader_set_position()` 本身只更新服务内存位置。
