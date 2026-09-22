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
| 存储 | TF 卡（/book、/font、/pic、/incoming）+ 内置 fs_root |

## 依赖
- SiFli-SDK v2.5+（已作为子模块 `SiFli-SDK/` 引入；也兼容用环境变量 `SIFLI_SDK` 指向任意 SDK 目录）
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
│   ├── main.c                     # 入口（数据服务初始化 + LVGL）
│   ├── ui/                        # 【UI 层】页面与交互框架（见 ui/README.md）
│   ├── services/                  # 【数据服务层】提供数据接口给 UI
│   │   ├── net/bt_pan.c|h         # 蓝牙 PAN 联网（手机热点共享）
│   │   ├── weather/weather.c|h    # 天气数据（快照/状态/事件）
│   │   ├── bookshelf/bookshelf.c|h# 书库管理（/book 扫描）
│   │   ├── reader/reader.c|h      # 阅读引擎（按偏移取文本/进度）
│   │   └── service_cmd.c          # msh 调试命令（svc，验证各接口返回）
│   └── boards/
│       ├── epd_e0470a03_57x/      # E0470A03 墨水屏驱动 + 波形逻辑 + TPS 电源
│       ├── touch/gt967/           # GT967 触摸驱动
│       ├── battery/               # 电量计
│       ├── controls/              # 按键（KEY1/2/3）
│       └── board_service.c|h      # 板级（文件系统/电源管理）
├── font/                          # 内置中文字体（DroidSansFallback，供 LVGL freetype）
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
- [ ] 切换 LVGL v9（当前为 v8 过渡分支）
- [ ] UI 框架（页面栈 / 焦点导航 / 低功耗状态机）— UI 同事负责
- [ ] 页面实现（锁屏/主页/书架/阅读器/设置/天气/文件传输/配网/关于）

## 开发规范
- 屏幕分辨率宏：`LCD_HOR_RES_MAX=684`、`LCD_VER_RES_MAX=1216`（竖屏，与驱动旋转逻辑对应）
- 波形模式（E0470A03）：`mode 0=GC16(全刷)`、`1=DU(局刷)`、`3=INIT`、`5=GL16`、`6=A2`、`7=DU4`
- 全刷周期控制：`set_part_disp_times(n)`（每 n 次刷新插入一次全刷）