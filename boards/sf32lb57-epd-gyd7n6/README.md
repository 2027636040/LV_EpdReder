# SF32LB57 EPD DevKit 板级配置

本配置用于 SF32LB57-DevKit-EPD 专用载板，主控为 SF32LB57GYD7N6，连接 E0470A03
墨水屏，界面采用 684×1216 竖屏布局。板型名称为 `sf32lb57-epd-gyd7n6_hcpu`。

## 硬件映射

引脚依据 `SF32LB57-DevKit-EPD_SCH_2026-10-08.pdf` 的 MCU、Panel、Panel PMU、
TF Card、Sensor & Key、Power 页及 `57x DevKit-EPD ioconfig-09052026.xlsx`。
主控和晶振装配依据同包 BOM。

| 功能 | MCU 引脚及配置 |
| --- | --- |
| 调试串口 USART1 | PA18 RX、PA19 TX |
| EPD 数据 D0～D7 | PA28、29、30、31、32、33、37、38 |
| EPD CLK、LE、OE、SPH | PA24、25、26、27 |
| EPD GMODE、CPV、STV | PA39、40、41 |
| TPS65185 I²C | I2C1，PA20 SCL、PA21 SDA，七位地址 0x68 |
| TPS WAKEUP、PWRCOM、PWRUP | PA42、43、44 |
| TPS GOOD | PA52，输入上拉 |
| TF 卡 SDMMC2 | PA47 CLK、48 CMD、49 D0、50 D1、45 D2、46 D3，四位总线 |
| TF 卡检测 | PA51，低电平表示插卡 |
| 触摸 I²C | I2C2，PA1 SCL、PA2 SDA，使用模组电源上的外部上拉 |
| 触摸 INT、RST、供电使能 | PA3、4、5 |
| ADC 按键扩展接口 | PA34，ADC channel 6，沿用三档电压识别 |
| RTC 晶振 | PA22、23，32.768 kHz；启用外部低速晶振 |
| 系统晶振 | 48 MHz |

SDK 的 `SOC_SF32LB57GYD7N6` 配置选择 16 MiB NOR 和 16 MiB PSRAM。
分区、链接脚本及内存布局沿用现有 DPI-HDK 版本。屏幕使用 E0470A03 驱动、
二进制波形及刷新结束后的 TPS 断电流程。新板使用 48 MHz、`SDMODE=0`、
`GDCLK_polarity=1`，行扫描参数为 `LSL=20`、`LBL=48`、`LEL=24`。
平台启动时先执行一次 INIT 清屏，再进入正常页面刷新。

SDMMC2 的 SDK 设备名是 `sd1`，平台通过 `EPD_SDCARD_DEVICE_NAME="sd1"`
绑定 SD 卡组件，挂载目录仍为 `/sdcard`。原 DPI/LCD 载板继续使用 `sd0`。
检测、挂载及应用退出握手继续由现有组件和平台负责。

## 触摸接入

触摸型号按 FT5446 配置，复用 SDK 的 `TSC_USING_FT5446U` 基本坐标读取驱动。
板级供电顺序为复位拉低、等待 100 μs、开启 PA5 电源、等待 1 ms、释放复位、
等待 200 ms；关闭时先拉低复位再关闭供电，并等待 5 ms。时序依据 FT5x46
规格书的上电和复位参数。

触摸通信使用 SDK 驱动的七位地址 `0x38`，通过 PA3
下降沿中断读取坐标，并由 SDK 触摸设备接入 LVGL 指针输入。

## 外设状态

EPD、触摸供电、TF 卡、串口、RTC、蓝牙 PAN 和开发板 ADC 按键接入现有平台。
Wi-Fi、AW99703 前光、音频功放和数字麦克风的供电使能初始保持低电平。
AW99703 使用 I²C 调光，未连接 PWM；本板不启用旧载板的 PWM 背光配置。
Wi-Fi 联网、前光调光及充电状态业务由相应驱动或服务后续接入。

## 构建

在仓库根目录激活 SDK，并按 [SD 卡组件说明](../../components/sdcard/README.md)
准备 `project/sf-pkg.yaml` 锁定的本地源码包。仅编译平台：

```powershell
. .\SiFli-SDK\export.ps1
scons -C project --board=sf32lb57-epd-gyd7n6_hcpu --board_search_path=../boards -j8
```

编译全部动态应用，同时生成天气和书架预装镜像：

```powershell
scons -C project --board=sf32lb57-epd-gyd7n6_hcpu --board_search_path=../boards APPS=all PREINSTALL=weather,books STORAGE_IMAGE=1 -j8
```

产物在 `project/build_sf32lb57-epd-gyd7n6_hcpu/`，完整应用包在其
`app-resources/` 目录。为本板单独更新应用：

```powershell
scons -C modules APPS=all FIRMWARE=build_sf32lb57-epd-gyd7n6_hcpu -j8
```

`STORAGE_IMAGE=1` 将 `fs_root.bin` 加入 SDK 下载清单，烧录该文件会覆盖内部文件系统。
保留设备数据时使用不含该选项的构建和下载清单。
