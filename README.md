# LV_EpdReder

基于 **RT-Thread + LVGL v9** 的墨水屏（EPD）阅读器工程。

目标功能见 [RT-Thread + LVGL 墨水屏 Demo 软件需求](RT-Thread%20+%20LVGL%20墨水屏%20Demo%20软件需求.html)：
锁屏 / 主页 Launcher / 书架 / TXT 阅读器 / 设置 / 文本设置 / 天气 / 城市选择 / 文件传输 / BLE Wi-Fi 配网 / 关于设备，支持 KEY1/2/3 导航与触摸。

## 硬件
| 项 | 规格 |
|---|---|
| 板卡 | SF32LB57GYD7N6（`customer/boards/dpi-hdk_lb57gyd7n6_epd`）|
| 屏幕 | E0470A03 4.7" 8bit EPD，1216×684（UI 竖屏 684×1216）|
| 波形 | 打库二进制波形（`EPD_WAVEFORM_USE_BIN`，SDMODE=1 / VCOM=-2.10V）|
| 输入 | KEY1/KEY2/KEY3 + GT967 触摸 |
| 存储 | TF 卡（FAT32，书籍直接放根目录）+ 内置 fs_root |

## 依赖
- SiFli-SDK 子模块，固定为 main 分支提交 `a25ebccdb986a98070287cf1701dc5adb10a265d`；通过环境变量 `SIFLI_SDK` 指向 SDK 目录
- 屏幕驱动来源：OpenSiFli/EPD_Reader PR#39（E0470A03，含波形表）

## 构建
```powershell
# 1. 激活 SiFli 环境（以本机 SDK 路径为准）
. <SiFli-SDK>\export.ps1

# 2. 编译
cd project
scons --board=dpi-hdk_lb57gyd7n6_epd --board_search_path=.. -j8

# 3. 烧录
project\build_dpi-hdk_lb57gyd7n6_epd_hcpu\uart_download.bat
```

> 构建时自动打包：`disk/` → fs_root 分区；`waveform/epd_waveform.bin` → wave_table 分区（256KB）。

## 目录结构
```
├── project/                       # SCons 构建工程
│   ├── SConstruct / SConscript
│   ├── Kconfig / Kconfig.proj     # 屏驱与工程配置
│   ├── proj.conf                  # 全局工程配置（含 LVGL 配置）
│   └── dpi-hdk_lb57gyd7n6_epd_hcpu/   # 板卡专属配置（proj.conf/ptab/link）
├── src/                           # 应用代码（分层：ui / services / boards）
│   ├── main.c                     # LVGL v9 显示、数据服务初始化与主界面事件循环
│   ├── ui/                        # Launcher、页面导航、状态模型和图标
│   ├── services/                  # 【数据服务层】提供数据接口给 UI
│   │   ├── net/bt_pan.c|h         # 蓝牙 PAN 联网（手机热点共享）
│   │   ├── weather/weather.c|h    # 天气数据（快照/状态/事件）
│   │   ├── bookshelf/bookshelf.c|h  # 书库管理（卡根目录扫描）
│   │   ├── reader/reader.c|h      # 阅读引擎（按偏移取文本/进度）
│   │   └── service_cmd.c          # msh 调试命令（svc，验证各接口返回）
│   └── boards/
│       ├── epd_e0470a03_57x/      # E0470A03 墨水屏驱动 + 波形逻辑 + TPS 电源
│       ├── touch/gt967/           # GT967 触摸驱动
│       ├── battery/               # 电量计
│       ├── controls/              # 按键（KEY1/2/3）
│       └── board_service.c/.h     # 文件系统和电源管理
├── assets/icons/                  # SVG 图标源文件
├── assets/ezip/                   # 固定尺寸透明 PNG，构建时转换为 EZIP
├── assets/SConscript              # SDK EZIP 资源生成与编译
├── tools/generate_ui_icons.cjs    # SVG → 灰阶透明 PNG
├── font/                          # 内置中文字体（DroidSansFallback，供 Tiny TTF）
├── waveform/                      # 打库波形 bin + 读取库
├── disk/                          # 内置文件系统镜像内容
└── RT-Thread + LVGL 墨水屏 Demo 软件需求.html
```

## 数据服务接口（给 UI）
- 蓝牙 PAN：`src/services/net/bt_pan.h` —— 状态查询 / 开关 / 事件
- 天气：`src/services/weather/weather.h` —— 请求 / 快照 / 状态 / 事件 / 城市
- 书库：`src/services/bookshelf/bookshelf.h` —— 扫描 / 列表 / 进度
- 阅读：`src/services/reader/reader.h` —— 打开 / 按偏移取文本 / 阅读位置
- 用法示例与线程约定：`src/services/README.md`
- 串口验证命令：`svc <service> <sub-command>`（直接输入 `svc` 查看全部）

## 开发状态
- [x] 新屏驱动与波形合入（来自 PR#39）
- [x] 板卡工程配置（dpi-hdk_lb57gyd7n6_epd_hcpu）
- [x] 板级服务（按键/触摸/电池/电源）
- [x] 纯 C 化（移除 C++ 抽象层）
- [x] 数据服务层：蓝牙 PAN 联网 + 天气接口框架
- [x] 数据服务层：书库（bookshelf）+ 阅读（reader）接口框架
- [x] msh 调试命令 `svc`（接口数据串口自验）
- [ ] 手机蓝牙 PAN 真机联调（天气全链路验证）
- [x] LVGL v9.4.0 显示适配、内置中文 TTF 字体和主界面
- [x] 有界页面栈、按键/触摸导航、返回时恢复焦点
- [x] 锁屏界面及 KEY2 / 触摸解锁
- [x] 书架、天气、文件传输、相册、Wi-Fi 配网、设置入口及返回承载页
- [x] 天气详情、三天预报、城市选择和更新提示界面
- [x] 书架卡片、阅读进度、分页边界和选书入口
- [x] 通用设置开关、循环选项和文本设置界面
- [ ] 子页面业务内容、设置后端和低功耗状态机

## 主界面

主界面采用 684×1216 竖屏布局，包含日期时间、无线状态、电量、
六个页面入口、最近阅读/天气/电量快捷信息和锁屏按钮。
KEY1 上选、KEY3 下选、KEY2 确认，长按 KEY1 返回上一级。
锁屏页面仅由 KEY2 或触摸“解锁”按钮退出，不执行硬件休眠或关机。

页面切换使用 LVGL v9 的即时屏幕加载，删除上一页对象树，
在最多 8 层的页面栈中保存页面编号和焦点位置；不使用滑动动画。
设置页支持通用设置和文本设置，保留关于设备入口；天气页支持城市选择，
书架页通过书籍卡片打开 TXT 正文。Wi-Fi 配网页面只保留提示和返回按钮。

状态栏的蓝牙/Wi-Fi 图标在关闭时隐藏；开启后分别使用未连接和已连接图标。
蓝牙状态来自 `btpan_get_state()`；关闭或协议栈尚未就绪时隐藏图标，
就绪但未连接时显示未连接图标，链路已连接或 PAN 已连通时显示已连接图标。
Wi-Fi 服务通过 `ui_app_set_wifi(enabled, connected)` 上报状态；
当前板卡工程未接入 Wi-Fi 驱动，默认关闭。
最近阅读和天气摘要分别通过 `ui_app_set_recent_reading()`、
`ui_app_set_weather_summary()` 更新；没有数据时显示空状态，不发起天气请求。
日期时间读取系统 RTC；RTC 时间未设置时显示占位文本。

电量 0–20% 使用空电池图标，21–79% 使用中段图标，80–100% 使用满电图标，
充电状态优先使用充电图标。负值或未知电量按 0% 处理。
SF32LB57X 的电池采集接口目前返回占位值 0，因此默认显示空电池和 0%。

图标使用编译进资源分区的 EZIP 压缩数据，通过 SDK 的 EZIP/EPIC 通路显示，不依赖 TF 卡。
PNG 源图片在构建时转换；工程启用 `LV_USE_EZIP`，关闭 `LV_USE_LODEPNG`，
当前不通过 LodePNG 读取文件系统中的原始 PNG。
中文字体使用 Tiny TTF 从内置 `DroidSansFallback.ttf` 的只读 Flash 数据加载，
字号为 20、24、28、36、64 像素，每个字号的字形缓存限制为 32 项；
64 像素字体用于天气温度，20 像素字体用于详情标题和辅助信息。
字体解析和缓存管理结构通过 LVGL 分配器使用 2 MiB 专用 PSRAM 堆；
字形位图通过 SDK 的 Tiny TTF 分配器使用独立的 256 KiB PSRAM 堆
（`TINY_TTF_CACHE_IN_PSRAM` / `TINY_TTF_CACHE_SIZE`）。
工程已启用 Tiny TTF 文件加载接口；主界面直接使用内置字体，不依赖 TF 卡。
文件读取缓存为每个打开的文件 4 KiB，使用 LVGL 分配器。
LVGL 输出警告及错误日志，并启用空指针、分配失败和样式初始化断言；
RT-Thread 开启线程栈溢出检查。
LVGL 的两个 40 行绘制缓冲区放在 PSRAM，合计 109440 字节，
为系统堆留出 SRAM；主线程和 LVGL 绘制线程各保留 32 KiB 栈。

### 天气页面

天气页按 684×1216 竖屏分为城市与更新时间、当前天气、详情卡片、三天预报和操作区。
详情卡片为两列四行，显示湿度、风速、能见度、云量、日出、日落、气压和空气质量；
底部保留数据来源、区域及城市选择、更新、返回按钮。

城市选择包括北京、上海、南京、深圳、杭州、成都、广州、西安。
KEY1/KEY3 移动焦点，KEY2 选中城市，点击“确认”后应用选择并返回天气页；
“取消返回”或长按 KEY1 不应用未确认的选择。更新提示支持触摸或 KEY2 关闭，
关闭后恢复“更新”按钮焦点。页面使用本地初始数据，尚未连接网络天气服务。

`assets/weather/` 包含 SuperKey 中完整的 70 个天气 PNG。
`ui_weather_icon()` 按和风天气代码查询，缺失代码回退到 `999`；
160×160 大图及 48×48 小图先生成近黑色 `#111111`、带 16 级边缘透明度的 PNG，再由 SDK 转换为 EZIP，
编译到资源分区。不需要运行时 PNG 解码、图像缩放或从 TF 卡加载。

### 书架页面

书架采用每页 4 张卡片的纵向布局，显示书籍封面图标、TXT 文件名、大小、
最近阅读时间、灰阶进度条和当前页/总页数；未读书籍显示“未读”。
书目来自当前挂载文件系统的根目录，支持 TF 卡或内置 NOR 中的 `.txt` 文件。
扫描上限为 32 本，按书名排序；文件名去掉后缀显示。

KEY1/KEY3 选择书籍，KEY2 或触摸卡片打开正文，返回时恢复书架页和焦点。
底部保留“上一页、主页面、下一页”，边界按钮置灰且跳过按键焦点。
空书库和存储不可用分别显示提示，重新进入书架时重新扫描。

### TXT 阅读

支持 UTF-8（含 BOM）和 GBK，自动检测，也可在文本设置里指定编码。
按原文件字节偏移保存位置，转码后仍能准确关联到原文；CRLF/CR 统一为换行，
分页使用与显示相同的 LVGL 字体度量，避免切断多字节汉字。

KEY1 上一页、KEY3 下一页、KEY2 返回书架，长按 KEY1 打开阅读设置。
触摸正文左侧翻到上一页、右侧翻到下一页、中间打开设置。
设置面板支持触摸翻页开关、全刷周期、±1/±5 调整目标页、确认跳转和文本设置入口。
字号、行距或边距改变后按原文位置重新分页；字体从 `/fonts/Song.ttf`、`Hei.ttf`、
`Kai.ttf`、`Monospace.ttf` 加载，文件缺失时回退到内置 DroidSansFallback。

阅读页只在内容或操作状态变化时更新正文与页脚，不显示分钟时钟，不使用动画或渐变。
普通翻页沿用局刷及周期全刷；打开/退出阅读页、阅读与文本设置页面切换时请求一次全刷。
屏幕刷新期间不积压翻页操作；首尾页不循环，也不因越界按键触发刷新。

首次打开时分批计算页码，总页数未完成时显示 `--`，不会为计算进度持续刷新屏幕；
下一次有效操作会带出已计算完成的总页数。分页索引缓存以文件路径、大小、修改时间
和排版参数校验，支持最多 65536 页，不将整本书加载到内存。
当前页起点、百分比和最近阅读时间保存在同一文件系统的 `/.epd_reader/` 中，
每次成功翻页写入两份交替记录之一并同步。写入失败时保留内存进度并在页脚提示。
重新打开书籍会恢复位置；最后一页记为 100%，空文件保持 0%。

### 设置页面

通用设置包含触控开关、超时关机、全刷周期、蓝牙、Wi-Fi、默认网络和低功耗模式，
并提供文本设置、关于设备入口。初始触控、Wi-Fi 和低功耗开关开启，蓝牙关闭，
超时为 5 分钟，全刷周期为 10 次，默认网络为 WiFi。

文本设置提供字体、字号、字重、行距、边距和文件编码；
默认值分别为 Default、24 px、正常、1.2x、8 px 和自动识别编码。
KEY1/KEY3 选择设置行，KEY2 或触摸整行切换开关或循环到下一选项。
开关位置和选项文字即时更新，不使用动画；进入子页面再返回时恢复原焦点。

设置项由 `ui_settings.c` 在内存中维护，页面往返时保留，重启后恢复默认值。
“保存设置”显示确认弹窗，关闭后恢复保存按钮焦点；“保存 ← 返回”及文本设置的
“确认返回”保留当前选择并返回上一级。阅读字体、字号、行距、边距和编码参与真实排版，
全刷周期控制屏幕驱动，触控开关控制阅读区域的触摸翻页。
字重、自动关机、蓝牙、Wi-Fi 和低功耗的硬件控制仍未接入。
状态栏仍显示设备服务上报的状态，不受设置页面开关影响。

### 资源生成

图标源文件位于 `assets/icons/`；其中相册图标为配套的线性矢量图。
安装 Node.js 的 `sharp` 模块后运行 `node tools/generate_ui_icons.cjs`，
生成 `assets/ezip/icons/` 下的固定尺寸透明 PNG 和图标声明。
天气图标使用 `node tools/generate_weather_icons.cjs` 生成 `assets/ezip/weather/` 中的
大小两套 PNG 及天气代码查询表，来源与布局见 `assets/weather/README.md`。
这些 PNG 输入文件随工程提供，普通固件构建不需要 Node.js；`assets/SConscript`
使用 SDK 的 `Env.ImgResource()` 自动转换为 LVGL v9 EZIP 资源。
SF32LB57X 使用 2 KiB 压缩窗口，生成的 C 文件与目标文件位于构建目录，
图像数据及描述符放入现有 ROM2 资源分区。

## 开发规范
- 屏幕分辨率宏：`LCD_HOR_RES_MAX=684`、`LCD_VER_RES_MAX=1216`（竖屏，与驱动旋转逻辑对应）
- 波形模式（E0470A03）：`mode 0=GC16(全刷)`、`1=DU(局刷)`、`3=INIT`、`5=GL16`、`6=A2`、`7=DU4`
- 全刷周期控制：`epd_wave_set_part_times(n)`；通过 `epd_wave_get_part_times()` 查询，`epd_wave_request_full()` 请求下一次全刷
