# SF32LB57 LCD DevKit 载板上的 E0470A03 墨水屏

本配置面向 `SF32LB57-DevKit-LCD_SCH_V1_2026-10-08` 载板，使用 SF32LB57GYD7N6 主控、16 MiB NOR Flash 和 16 MiB PSRAM。显示设备仍为项目使用的 E0470A03 墨水屏，不是 RGB LCD。

## 构建

在仓库根目录激活 SDK 后执行一条构建命令即可生成平台及天气、书架预装镜像：

```powershell
. .\SiFli-SDK\export.ps1
scons -C project --board=sf32lb57-lcd-gyd7n6_epd_hcpu --board_search_path=../boards PREINSTALL=weather,books STORAGE_IMAGE=1 -j8
```

不需要预装时，去掉 `PREINSTALL` 和 `STORAGE_IMAGE`。`--board_search_path` 相对于 `project`，使用 SDK 的项目外置板级搜索机制。

产物位于 `project/build_sf32lb57-lcd-gyd7n6_epd_hcpu/`，应用安装包位于该目录的 `app-resources/`。仅更新应用时，明确指定这块板的平台产物：

```powershell
scons -C modules APPS=weather,books FIRMWARE=build_sf32lb57-lcd-gyd7n6_epd_hcpu -j8
```

烧录脚本也位于新板构建目录；`STORAGE_IMAGE=1` 会将 `fs_root.bin` 加入清单，烧录后覆盖内部文件系统。不要混用旧 DPI-HDK 目录的固件和应用安装包。

## 引脚适配

映射依据为载板原理图的 LCD、TF、触摸、扩展器页面，以及 `57x+LCD-DevKit+ioconfig-08192026.xlsx`。转接板侧沿用旧 DPI 接口的信号用途，板级代码只改变 MCU 到接口的引脚映射。

| 功能 | LCD DevKit 引脚 |
| --- | --- |
| I2C1 SCL / SDA | PA22 / PA23 |
| TF CLK / CMD / D0 | PA8 / PA9 / PA10，一位总线 |
| TF 插拔检测 | AW9523B P0.1，低电平表示插卡 |
| AW9523B INTN / 地址 | PA21 / 7 位地址 `0x59` |
| 触摸 INT / RESET | PA20 / AW9523B P1.6 |
| TPS WAKEUP / PWRCOM / PWRUP | PA51 / PA24 / PA25 |
| EPD 电源 / 触摸电源 / GMODE | PA52 / PA53 / PA54 |
| 三键 ADC | PA34，ADC 通道 6 |

FPC1 的 RGB 信号对应如下，`bsp_lcd_tp.c` 按旧 DPI-HDK 的接口用途配置 TCON：

| 接口信号（按顺序） | PA 引脚（按顺序） |
| --- | --- |
| R0～R7 | 24、25、26、27、28、29、30、31 |
| G0～G7 | 32、33、37、38、39、40、41、42 |
| B0～B7 | 43、44、45、52、53、54、46、47 |
| CLK / HSYNC / VSYNC / DE | 48 / 49 / 51 / 50 |

## 代码职责

- 复用 SDK 的 `sf32lb57-dpi-hdk_base` 时钟、Flash/PSRAM 初始化和通用供电实现；通过独立构建目录分别生成主程序与 bootloader 的目标文件。
- 项目侧实现载板引脚和 AW9523B 适配。扩展器在 I2C 注册后、SDIO 初始化前启动；触摸复位不依赖 SD 卡服务是否启用。
- AW9523B P0.1 通过板内虚拟 PIN `129` 接入现有 SD 卡组件。PIN 适配只接管这一编号，其余 GPIO 操作仍交给 SDK。中断先将卡标记为不可用，再唤醒线程读取两个输入端口、清除扩展器中断并发布实际电平；不在中断或关中断区域访问 I2C。
- 扩展器访问失败时，检测状态保持为无卡；运行中的 I2C 错误在线程中重试。设备初始化失败时不启动插拔检测。
- 链接脚本和内存布局复用旧墨水屏项目配置，分区表保持一致。E0470A03 驱动、波形、刷新时序、UI 分辨率和业务代码不因载板切换而改变。

## 验证

`tools/tests/run_lcd_board_tests.py` 在 MSVC 开发者环境中检查 TCON 映射、分区一致性，以及模拟扩展器下的 GPIO 转发、触摸复位、插拔检测和失败清理。测试不访问硬件。

上板需验证启动显示、全刷/局刷、触摸复位和输入、启动有卡/无卡、运行中拔卡退出及重新插卡挂载。编译和模拟测试不能替代这些电气与时序验证。
