# 技术储备索引

本索引区分当前实现、待实施方案和已经取消的目标。应用当前行为以各自设计及其源码入口为依据，平台阶段安排见[平台路线图](PLATFORM_ROADMAP.md)。标为待实施的接口和状态流程属于后续设计，不表示已编入固件或通过设备验证。

## 当前动态应用

| 应用 | 设计入口 | 当前能力与边界 |
| --- | --- | --- |
| 天气 | [weather/DESIGN.md](../modules/weather/DESIGN.md) | 已实现配置、城市历史、离线缓存和独立后台同步，公共 HTTPS 已复用。 |
| 书架 | [books/DESIGN.md](../modules/books/DESIGN.md) | 已实现 TXT、Markdown、EPUB、MOBI，以及分卷 EPUB 和 KF7/KF8 兼容路径；已接入文件交接，具体文件入口与排版限制见设计。 |
| 图库 | [gallery/DESIGN.md](../modules/gallery/DESIGN.md) | 已实现双存储浏览、PNG/JPEG、统一 16 灰阶和两个图片缓存槽，并接入文件交接。 |
| 单词 | [words/DESIGN.md](../modules/words/DESIGN.md) | 已实现 WDB2 统一词库、查词和 FSRS-6 学习；标准完整词库随包安装到 TF 卡。 |
| 微信读书 | [weread/DESIGN.md](../modules/weread/DESIGN.md) | 已实现登录、书架、目录、正文和账号隔离缓存；v3 导航修复已编译，板上目录与正文交互待确认。 |
| 触摸示例 | [hello/DESIGN.md](../modules/hello/DESIGN.md) | 展示页面计数、子页恢复及独立受管后台，不包含持久化业务。 |
| 内容看板 | [board/DESIGN.md](../modules/board/DESIGN.md) | 已实现配置驱动卡片、平台摘要展示和同源几何预览；两板编译及打包通过，实机生命周期与显示仍需验证。 |

## 技术储备

| 主题 | 状态 | 文档与实施前提 |
| --- | --- | --- |
| 系统低功耗与关机 | 待实施，已有面板断电及服务收尾基础 | [PM 接入](reserves/PM_INTEGRATION.md)说明当前缺口；[完整 PM 储备](PM_TECHNICAL_RESERVE.md)保留 SDK 机制、状态机及硬件验证设计。 |
| 阿里云语音输入 | 待实施 | [语音输入](reserves/SPEECH_INPUT.md)覆盖采音、双麦、云端协议、文字交接和凭证；本板麦克风通路及准确率尚待实测。 |
| 固件空中升级（OTA） | 独立离线安装程序待实施，当前分区无预留槽 | [OTA 与恢复](reserves/OTA.md)保留独立安装程序及文件暂存方向，明确主程序、资源和启动链边界；应用包更新不等于固件 OTA。 |
| Wi-Fi 接入 | 待实施，统一网络接口已具备 | [Wi-Fi](reserves/WIFI.md)说明网卡登记、DNS、网络代次及配网职责；当前实际业务使用蓝牙 PAN。 |
| 图片解码演进 | 主要目标已实现，部分硬件路径仍为候选 | [图片技术储备](IMAGE_DISPLAY_TECHNICAL_RESERVE.md)保留后端研究，当前实现见图库设计；JPEGD→EPIC 直接缩放尚未作为图库默认路径。 |
| 应用能力演进 | 已实现部分与候选分开维护 | [应用演进](reserves/APPLICATION_EVOLUTION.md)汇总阅读、词库、微信读书及其他仍有效的业务储备。 |
| TF 卡组件 | 已有独立设计与实施记录 | [SD 卡组件清单](SD_CARD_COMPONENT_TODO.md)与[组件说明](../components/sdcard/README.md)维护具体状态。 |

## 已取消或被替代的目标

| 原目标 | 当前边界 |
| --- | --- |
| 为本地阅读增加 PDF、DOC、DOCX、FB2 等格式 | 已取消；只维护 TXT、Markdown、EPUB、MOBI 及当前兼容族。 |
| 普通灰阶、16 灰阶与抖动三模式比较及切换 | 已取消；图库、书籍插图和微信读书图片统一输出 16 灰阶。 |
| 应用内存按归属追踪及其扩展统计体系 | 已取消；对象由谁创建、持有和释放仍是生命周期设计的一部分，不代表恢复内存归属统计。 |
| 按多个独立词书文件组织新词学习 | 已由 WDB2 统一词库和范围成员索引替代；旧格式读取仅保留必要兼容。 |
| 将旧微信读书 UI、线程和存储整体移入平台 | 已由独立动态应用、公共 HTTP 和公共阅读组件替代。 |

## 配套入口

[构建说明](BUILDING.md)定义平台、应用包和预装输出；[发布说明](RELEASING.md)定义交付材料；[存储迁移](STORAGE_MIGRATION.md)说明内部数据迁移；[平台实施清单](PLATFORM_IMPLEMENTATION_TODO.md)维护平台工作状态。看板参考应用及同源预览见[看板设计](../modules/board/DESIGN.md)，已实现的文件交接见[跨应用内容打开](CONTENT_HANDLERS.md)。
