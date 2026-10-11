# 天气应用设计

## 目的与职责

天气应用以 `weather.so` 提供实况、三日预报、空气质量、城市设置和查询历史，并向主页发布天气摘要。应用拥有和风业务协议、配置、缓存及页面；平台负责应用装载、网络选择、文件系统、服务停止和显示。运行入口与安装元数据见 [app_main.c](app_main.c) 和 [app.json](app.json)。

当前清单版本为 3、数据版本为 1，声明 `background: true` 和设置入口。后台同步与天气页面的存续分开：应用安装且存储可用时，平台可以启动后台；卡内应用还需用户确认加载。关闭页面后继续同步。

| 部分 | 职责与执行位置 |
| --- | --- |
| [weather.c](weather.c) | 在单个受管工作线程内执行同步、配置导入和城市切换；维护互斥保护的业务状态。 |
| [weather_http.c](weather_http.c) | 配置和风请求头及响应大小限制，调用[公共 HTTPS](../common/http/epd_http.h)。 |
| [weather_store.c](weather_store.c)、[weather_history.c](weather_history.c) | 保存配置、有效天气和按城市组织的历史记录。 |
| [app_main.c](app_main.c)、[weather_view.c](weather_view.c)、[weather_settings.c](weather_settings.c) | 在 UI 线程内管理框架页面、控件、弹窗、草稿和请求结果。 |
| [ui_weather_data.c](ui_weather_data.c) | 将业务快照转换为页面文字和资源选择，比较变化后更新。 |

## 生命周期与状态

模块装载时，`module_init()` 创建互斥锁和请求队列，加载配置、缓存及城市索引。页面与后台都通过这份模型工作。平台停止后台并释放页面、命令等引用后，`module_cleanup()` 才销毁队列、索引和锁。

根页面默认为天气；带 `page=settings` 的启动参数将设置页作为根页。城市二维码、数字输入和历史列表均由 app_fwk 创建子页。框架拥有 screen、控件树和页面上下文，应用不另建导航栈。

| 页面阶段 | 应用动作 |
| --- | --- |
| `ONSTART` | 创建控件和输入组，建立并暂停 250 ms 定时器。 |
| `ONRESUME` | 读取最新模型、恢复页面专用资源和定时器，消费本页请求结果。 |
| `ONPAUSE` | 暂停定时器；天气页释放文件图像，城市输入页保存草稿。后台服务继续运行。 |
| `ONSTOP` | 删除定时器，释放历史页自有结果；控件树及页面内存由框架回收。 |

后台按网络事件及下一业务期限等待。开机、网络恢复或网络代次改变后保留同步需求；正常周期为 30 分钟，失败后等待 5 秒再尝试，离线期间不发请求。进入天气页面本身不触发额外联网。

业务快照状态为 `IDLE`、`REFRESHING`、`UPDATED`、`CACHED` 或 `FAILED`。同步失败时，有旧数据则保留旧快照并标记缓存状态，无旧数据才显示失败。状态与前台任务结果分开，静默同步不会结束另一个前台操作的等待弹窗。

## 数据所有权与接口

[weather.h](weather.h) 定义以下模块内部接口：

- `weather_request_job()` 复制请求并返回操作编号 `ticket`；同一时刻只受理一个前台任务。调用方根据编号识别自己的结果。
- `weather_get_info()`、`weather_get_config()`、`weather_get_result()` 在锁内复制快照；返回结构中的字符串属于副本，不指向 JSON 或工作线程缓冲。
- `weather_request_refresh()`、`weather_set_city()` 供命令等入口排队，不在调用线程执行 HTTPS。

工作线程是配置和天气数据的写入者；互斥区只保护模型，不包住网络或持久化操作。新数据增加修订号 `revision`。UI 使用副本更新控件，主页读取平台保存的摘要副本，无需调用天气模块中的函数。城市历史的动态索引由模块持有，页面仅领取一页副本。

公共 HTTPS 在工作线程同步完成请求，套接字与 TLS 阶段支持取消和网络代次检查。SDK 同步 DNS 只能在调用前后检查取消。天气适配器设置 30 秒请求期限、32 KiB 响应限制；HTTP 状态与业务 JSON 均有效才继续解析。公共 HTTP 响应由 `epd_http_response_clear()` 释放，天气适配器返回的正文副本由原天气调用方使用 `rt_free()` 释放。

## 持久化与失败处理

应用通过 `storage_app_path()` 获取路径。内部安装对应 `/flash`，TF 卡安装对应 `/sdcard/.epd`；以下目录均位于应用安装卷。

| 数据 | 相对安装卷路径 | 提交条件 |
| --- | --- | --- |
| API 配置与当前城市 | `data/weather/config.bin` | 导入或城市操作保存成功后才替换内存配置。 |
| 当前天气 | `cache/weather/current.bin` | 实况、预报及本地保存成功后发布完整快照；空气质量缺失可以降级。 |
| 城市历史 | `data/weather/cities/<城市ID>.bin` | 城市验证、信息变化或选择顺序变化时写入，周期同步不重复写相同记录。 |

配置导入只读取 `/sdcard/qweather.json` 中的 `api_host` 和 `api_key`。配置不随应用包更新而覆盖，密钥不进入普通日志。记录使用版本、CRC 校验及平台文件替换机制；加载天气时核对城市，避免显示上一个城市的数据。

从历史切换城市可以离线完成，不重复查询城市接口；切换成功后清除其他城市的天气，安排联网后的静默同步。更新、卸载或存储退出时，平台取消后台工作并等待线程和在途命令结束，然后释放模块。TF 卡移除会统一停止动态应用，内部安装的天气也先参与退出，清理完成后可以重新运行。卸载保留配置等用户数据，清理可重建缓存。

## 构建与部署

在仓库根目录激活 SDK，并先构建配套平台：

```powershell
. .\SiFli-SDK\export.ps1
scons -C project --board=dpi-hdk_lb57gyd7n6_epd_hcpu -j8
scons -C modules APPS=weather -j8
```

已有兼容平台时只需执行应用构建。安装包位于 `project/build_dpi-hdk_lb57gyd7n6_epd_hcpu/app-resources/weather/`，将完整目录复制到 TF 卡 `apps/weather`，通过系统应用安装入口安装或更新。LCD 载板的应用构建增加 `FIRMWARE=build_sf32lb57-lcd-gyd7n6_epd_hcpu`。构建配置标识、预装和存储规则见[构建说明](../../docs/BUILDING.md)。

## 验证入口与演进

模块运行时可用 `svc weather status`、`svc weather refresh`、`svc weather city [id]` 观察请求与快照。公共传输的主机入口为 [run_http_tests.py](../common/http/tests/run_http_tests.py)；资源部署和设备检查见[天气应用资源](../../docs/WEATHER_RESOURCES.md)。设备验证应覆盖离线缓存、城市切换、网络恢复及同步中退出或移除安装卷。

网络事件等待、持久化和后台停止已接入；系统休眠仍属于 [PM 技术储备](../../docs/reserves/PM_INTEGRATION.md)。页面定时检查不能作为事件驱动休眠已经完成的依据。
