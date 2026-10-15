# LV_EpdReder

基于 SiFli SDK、RT-Thread 和 LVGL V9 的墨水屏应用平台。主固件提供桌面、设置、文件管理、存储、网络和动态应用运行支持；天气、书架、图库、单词、微信读书及信息看板分别构建和安装。

## 文档入口

- [网页知识库](doc/index.html)：离线阅读、分类导航、全文搜索与页内目录。
- [开发意图与设计决策](docs/DESIGN_INTENT.md)：目标、选择理由、代价、历史变更及当前差距。
- [平台架构](docs/ARCHITECTURE.md)：分层、执行流程、数据和资源所有权。
- [开发文档索引](docs/README.md)：应用开发、部署、诊断与各应用设计。
- [平台与应用构建](docs/BUILDING.md)：环境、板型、应用选择和工厂预装。
- [平台演进](docs/PLATFORM_ROADMAP.md)与[技术储备](docs/TECHNICAL_RESERVES.md)：已实现能力与后续设计。
- [实施与验证清单](docs/PLATFORM_IMPLEMENTATION_TODO.md)：本轮集成状态及上板检查。

## 硬件与依赖

| 项目 | 当前参考配置 |
| --- | --- |
| 主控 | SF32LB57GYD7N6 |
| 载板 | EPD DevKit 专用载板；保留 SDK DPI-HDK 和项目内 LCD DevKit 配置 |
| 显示 | E0470A03，UI 为 684×1216 竖屏；三种载板均连接墨水屏 |
| 存储 | 内部 NOR 使用 LittleFS，TF 使用 FatFs |
| 网络 | 蓝牙 PAN；Wi-Fi 适配接口已预留，具体驱动尚未接入 |
| 输入 | 平台统一触摸与开发板按键，实际引脚和触摸装配按板型确定 |

SDK 通过仓库子模块与本地补丁管理。屏幕驱动和波形位于
`src/boards/epd_e0470a03_57x/`。
SD 组件由 `project/sf-pkg.yaml` 声明，准备方法见 [组件说明](components/sdcard/README.md)。
精确构建配置记录在板型输出目录的 `app-profile.json` 中。

## 快速构建

首次克隆后，在仓库根目录的 PowerShell 中准备锁定的 SDK 和 SD 组件，再构建：

```powershell
git submodule update --init --recursive
. .\SiFli-SDK\export.ps1
python tools/prepare_build.py
scons -C project --board=dpi-hdk_lb57gyd7n6_epd_hcpu -j8
```

`prepare_build.py` 应用仓库内的 SDK 补丁，并通过 Conan 和 SDK 原生入口安装 SD 组件。
SDK 基线、补丁或组件更新后重新执行；普通增量编译直接运行 SCons。
不要把子模块更新到远端最新版本。若已进入 `project/` 目录，构建命令省略 `-C project`。

默认只编译平台，不依赖动态应用源码、单词词库或 Node.js。SDK 原生工具生成
`fs_root.bin`，普通下载清单不包含这个文件系统镜像。

LCD DevKit 使用：

```powershell
scons -C project --board=sf32lb57-lcd-gyd7n6_epd_hcpu --board_search_path=../boards -j8
```

载板映射见 [LCD DevKit 说明](boards/sf32lb57-lcd-gyd7n6_epd/README.md)。

EPD DevKit 专用载板使用：

```powershell
scons -C project --board=sf32lb57-epd-gyd7n6_hcpu --board_search_path=../boards -j8
```

引脚、触摸接入及配套应用构建见 [EPD DevKit 说明](boards/sf32lb57-epd-gyd7n6/README.md)。

### 动态应用

已有配套平台后，可以单个、批量或全部构建：

```powershell
scons -C modules APPS=weather -j8
scons -C modules APPS=books,gallery -j8
scons -C modules APPS=all -j8
```

`APPS` 支持 `weather,books,gallery,words,weread,board`。默认使用 DPI 板固件；
为 LCD 板构建时增加 `FIRMWARE=build_sf32lb57-lcd-gyd7n6_epd_hcpu`。
完整安装包位于 `project/build_<board>/app-resources/<id>/`。
平台配置变化后需要配套重编应用，不要单独复制 `.so`。

单词应用的全量预生成词库已纳入 Git；构建自动解压，不重新下载原始数据。
图标生成需要 Node.js 和 `sharp`，准备方法见 [单词应用](modules/words/README.md)。

### 工厂预装

一次构建平台、天气和书架，并把两个完整包放入出厂文件系统：

```powershell
scons -C project --board=dpi-hdk_lb57gyd7n6_epd_hcpu PREINSTALL=weather,books STORAGE_IMAGE=1 -j8
```

不必先执行一遍 `APPS=weather,books`。烧录入口为同一构建目录中的
`uart_download.bat` 或 SDK 生成的其他下载脚本。
`STORAGE_IMAGE=1` 会将 `fs_root.bin` 加入下载清单；烧录它会覆盖内部文件系统。
保留用户数据的普通固件更新不使用此选项。

## 应用与公共组件

| 应用 | 功能 | 设计文档 |
| --- | --- | --- |
| weather | 天气、城市历史、独立后台同步 | [设计](modules/weather/DESIGN.md) |
| books | TXT、MD、EPUB、MOBI 阅读与进度 | [设计](modules/books/DESIGN.md) |
| gallery | PNG/JPEG 等比显示与 16 灰阶处理 | [设计](modules/gallery/DESIGN.md) |
| words | 统一离线词库、学习范围和 FSRS-6 复习 | [设计](modules/words/DESIGN.md) |
| weread | 登录、在线书架、章节缓存和阅读 | [设计](modules/weread/DESIGN.md) |
| board | 配置式信息卡片与公共摘要 | [模板与预览](docs/BOARD_TEMPLATES.md) |
| hello | 页面导航与受管服务开发示例 | [设计](modules/hello/DESIGN.md) |

公共 HTTP、图片和阅读视图源码位于 `modules/common/`。
动态应用通过 app_fwk 管理页面，通过平台接口使用存储、网络、字体与受管后台服务。
主固件不负责各应用的城市、词条或书籍业务规则。

## 存储、显示与诊断

应用安装在内部存储或 TF 卡，业务数据和缓存跟随安装位置。TF 卡移除会触发会话失效与协作退出；
再次插入后通过加载确认启用卡内应用。安装及迁移规则见 [应用开发](docs/APPLICATION_DEVELOPMENT.md)。

页面切换触发全刷并重置周期计数；同页内容更新保留全刷周期；状态栏小范围自动更新不累计周期。
应用不控制扫描时序或波形。字体采用内置 MiSans，压缩资源启动解压至 PSRAM，字形像素按需缓存。

动态模块共享系统地址空间。故障定位使用精确符号与模块地址，见
[动态应用故障取证](docs/DYNAMIC_APP_DEBUGGING.md)；问题应用的停用与恢复见
[恢复操作](docs/RECOVERY.md)。

低功耗、离线 OTA、语音输入等尚未实施内容，统一在[技术储备](docs/TECHNICAL_RESERVES.md)中说明，
不以设置占位项或方案文档作为功能已经完成的依据。
