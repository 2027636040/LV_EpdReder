# 天气动态应用

## 组成与职责

`weather.so` 包含天气页面、天气设置、城市 ID 二维码与输入页、HTTPS/JSON 业务和持久化。
它不依赖主固件的 `ui_internal.h` 或内置页面 ID。

| 文件 | 职责 |
|---|---|
| `app_main.c` | app_fwk 根页、子页、页面内存、焦点组、弹窗和定时器生命周期 |
| `weather_view.c` | 天气布局、差量控件更新、手动同步结果提示 |
| `weather_settings.c` | TF 配置导入入口、二维码、数字输入与草稿 |
| `weather_history.c` / `weather_history_page.c` | 城市历史持久化、互斥快照和分页切换列表 |
| `ui_weather_data.c` / `weather_icons.c` | 显示数据转换与私有资源选择 |
| `weather.c` | 单个受管后台线程、30 分钟同步、5 秒重试和互斥快照 |
| `weather_http.c` / `weather_store.c` | 请求、响应解压、配置和缓存文件 |
| `weather_command.c` | 模块运行期间的 `svc weather` 命令 |

平台仅提供 app_fwk、通用控件/字体/图像、网络、存储、受管线程及公共库导出。
`module_init` 在页面或后台任务使用模型前创建互斥锁/请求队列并加载缓存；
`module_cleanup` 在页面和后台引用都释放后清理它们。

页面 ONSTART 创建控件与暂停的定时器，ONRESUME 读取最新快照并恢复更新，
ONPAUSE 暂停页面定时器、释放文件图像并保存未提交输入，
ONSTOP 删除定时器和焦点组。screen、子控件和页面内存由框架释放。
250 ms 定时器只读取前台结果，文字和图片相同时不触发重绘；没有动画或闪烁光标。

普通入口的根页是天气，`page=settings` Intent 的根页是天气设置；
设置 → 二维码 → 数字输入，返回逐级退出，城市验证成功返回天气设置根页。
导航和返回只使用 app_fwk，没有模块自建页面栈。
新模块使用触摸交互，不接入开发板按键分发。

`background: true` 使后台在安装后和开机枚举后启动，不依赖天气页面。
平台持有额外模块引用；返回桌面只退出页面，不停止同步。
更新、卸载或存储切换时，平台先停止页面、通知后台取消请求并等待退出，再释放模块。
模块不保留跨生命周期的 LVGL 指针；串口命令在途时，平台也会等命令返回后才释放引用。

## 构建与安装

在仓库根目录执行：

```powershell
. .\SiFli-SDK\export.ps1
Set-Location project
scons --board=dpi-hdk_lb57gyd7n6_epd_hcpu -j8
```

固件构建自动生成模块编译配置，构建本目录并生成完整安装包。
模块产物为 `modules/weather/output/weather.so`，安装目录为
`project/build_dpi-hdk_lb57gyd7n6_epd_hcpu/app-resources/weather`。
打包后自动核对模块外部符号与主固件 RTM 导出表。

将安装目录完整复制到 TF 卡 `apps/weather`，再从设置的应用安装入口安装“天气”。
普通固件下载不覆盖内部文件系统，单独更新固件不会将旧资源包自动变为可执行应用。
旧天气资源包版本 1 和动态应用版本 2 可更新到此模块包版本 3；配置数据版本仍为 1，更新保留配置。
天气包卸载后后台停止、入口消失，保留配置数据，清理可重建缓存。

资源部署、文件所有权和上板检查见
[天气应用资源](../../docs/WEATHER_RESOURCES.md)。

## 天气数据与线程

设备通过统一网络服务连接和风 HTTPS 服务，不经过上位机；当前实际网络来源是蓝牙 PAN。
网络服务在 lwIP 线程中检查所选网卡的 Link、IP 和网关；自动同步保留待处理状态，
直到网络就绪后才开始请求。手动同步和城市校验通过事件等待网络，等待时间最多约 15 秒。
DNS 使用 lwIP 的线程安全解析接口。TLS 连接、握手及响应读取采用非阻塞套接字和 30 秒截止时间，
保留 SDK 的 CA 链及域名校验；不跟随重定向，不在 URL 或日志中输出 API Key。
TLS 的证书有效期检查仍取决于 SDK 的 MBEDTLS_HAVE_TIME_DATE 配置。

单个受管 weather 线程顺序执行网络请求、城市校验、TF 配置导入和应用数据写入。
空闲时按下一业务期限等待网络、手动请求或停止事件，页面退出不会停止该线程。
UI 使用 weather_get_info() 复制由互斥锁保护的完整快照；锁内不执行网络或 Flash 操作。
发布数据时增加 revision，ui_weather_data.c 在 UI 线程转换显示文字，只有值发生变化的控件才重绘。
工作线程发布平台持有的摘要副本，主页无需调用天气模块代码；天气页读取同一份业务快照。
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
开机先恢复本地缓存，配置和城市有效且所选网络取得 IP 和网关后同步一次，
断线重连后补发，并保留固定 30 分钟周期；离线时到期的同步任务不会被丢弃。
天气和 NTP 分别订阅网络事件，使用网络快照中的代次识别重连与切换；请求仍在各自工作线程执行。
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

实况和预报沿用现有城市天气 v7 数据模型。

### TF 配置与持久化

TF 卡根目录放置 UTF-8 的 qweather.json，固定两个字符串字段：

```json
{
  "api_key": "YOUR_API_KEY",
  "api_host": "YOUR_API_HOST.qweatherapi.com"
}
```

设置 → 应用管理 → 应用设置 → 天气 → 从 TF 卡导入天气配置。
只读取 `/sdcard/qweather.json`，不把 NOR 同名文件当成 TF 配置；
挂载统一由存储服务负责，未挂载时导入失败，不自动格式化存储设备。
导入不立即验证密钥，城市校验或天气同步时由服务器验证。保存失败不替换现有配置。

城市设置先显示 LVGL 二维码，地址为和风官方城市列表页；点击“我已获取城市ID”后进入数字键盘。
输入 ID 必须与 GeoAPI 返回的 ID 一致，网络校验和保存成功才改变城市，并清除其他城市的缓存。
配置和缓存分别保存在应用安装卷的 `data/weather/config.bin`、`cache/weather/current.bin`，
内部路径以 `/flash` 开头，TF 卡路径以 `/sdcard/.epd` 开头。加载时核对缓存对应的城市。
两者使用版本/CRC 校验、临时文件写入和 fsync，FAT 通过备份旧文件支持替换恢复。
内部安装在移除 TF 卡后仍可使用已导入配置；卡内安装的数据随卡携带，拔卡后停止天气线程。

### 查询历史

天气设置中的“查询历史”列出已经验证或成功获取实况的城市，按城市 ID 去重，最近选择的城市排在前面。
列表每页显示六个城市，标出当前城市；点击后恢复已保存的名称、区域和经纬度，保存设置并返回天气设置。
历史切换不重复请求 GeoAPI，也不要求当前联网。切换到不同城市时先清除旧城市的天气快照，
由后台在联网后静默获取新城市天气；未获得新数据时不会显示其他城市的天气。

每个城市使用应用数据目录的 `cities/<城市ID>.bin` 独立保存，沿用版本/CRC 校验和文件替换接口，
记录不含 API Key。历史不会按固定条数淘汰；实际容量由应用安装卷和可用 PSRAM 决定。
模块加载时在 PSRAM 建立动态索引，模块卸载时释放；UI 只读取一页副本，不持有后台链表指针。
只有新增城市、城市信息改变或手动选择导致排序变化时写入，周期天气同步不会重复写历史。
升级后会补入原有配置中的当前城市，旧版本未保存过的其他城市无法追溯恢复。

参考：[请求配置](https://dev.qweather.com/docs/configuration/api-config/)、
[城市查询](https://dev.qweather.com/docs/api/geoapi/city-lookup/)、
[城市实况](https://dev.qweather.com/docs/api/weather/weather-now-webapi-v7/)、
[城市预报](https://dev.qweather.com/docs/api/weather/weather-daily-forecast-webapi-v7/)、
[空气质量](https://dev.qweather.com/docs/api/air-quality/air-current/)。

## 串口检查

仅在天气模块已安装且受管服务运行时提供：

```text
svc weather status
svc weather refresh
svc weather city [id]
```

主固件的命令分发不依赖天气私有类型；调用期间保持模块引用，
卸载停止后不接受新命令，等待在途命令返回。
