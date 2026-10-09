# SF32LB57 墨水屏阅读器 PM 接入技术储备

## 1. 结论与适用范围

本项目采用两种明确分开的产品行为：锁屏保留运行状态，追求快速恢复；关机停止业务运行，不依赖 RAM/PSRAM 数据保留，下次开机重新初始化并加载持久化数据。产品状态名 `DeepSleep` 对应后一种关机行为，本文同时用 `OFF` 标识。

锁屏不要求统一关闭蓝牙、TF 卡等外设。锁屏画面绘制完成后，面板按现有流程断电，不再因时间、天气或无线状态变化刷新屏幕；独立后台服务按原有规则运行。处理器在没有任务时仍可休眠，保留外设不等于让 CPU 持续运行。

电源管理（Power Management，PM）负责让无任务的处理器及符合条件的设备降低功耗。锁屏需要保留内存，可能还保持无线连接，因此即使关闭触摸，也不能据此认为锁屏功耗等于 OFF。

关机只保存需要跨启动恢复的业务数据，不保存整块内存镜像。LVGL 对象、app_fwk 页面上下文、线程、网络连接、字形缓存和显示缓冲在下次启动时重新建立。硬件仍需保留能够检测开机按键、充电事件的常供电部分；“不保留 RAM 数据”不等于已经证明所有 RAM 电源轨都已关闭。

目前系统 PM 尚未启用。已有面板断电、输入合并、差异刷新和缓存写回机制是接入基础。墨水屏波形、扫描时序和供电时序不属于本方案的调整对象。

研究对象为 SF32LB57GYD7N6、E0470A03、LVGL V9，板级目标为 `dpi-hdk_lb57gyd7n6_epd_hcpu`。SDK 固定提交为 `a25ebccdb986a98070287cf1701dc5adb10a265d`。项目基线为提交 `39578298327c6cc222284b8fb006ff2f4ee5aa43` 加当前工作区中的 app_fwk、输入和显示等修改，因此仅检出该提交并不能重现本文描述的全部行为。SDK 工作区另外保留缓存比较类型和 TLS 证书修改。

本文的“已确认”指源码和现有构建配置能够直接证明的行为；“建议”是后续接入方案；“待验证”需要启用 PM 后编译、上板或测量。现有构建未启用 PM，不能作为 PM 配置的编译或运行验证，也没有可用于承诺续航的电流测量结果。

### 1.1 产品状态与转换规则

产品交互状态与 SDK 睡眠深度分别管理。锁屏是仍在运行的系统中的页面状态；`ScreenSaver` 是关机前显示关机画面的阶段，两者独立。

| 产品状态 | 屏幕、内存和业务行为 | 离开条件 |
|---|---|---|
| ACTIVE | 有刷新请求时保持必要运行性能，完成绘制与发送；后台计算也可使 CPU 运行，但不必引起刷屏 | 当前工作完成后立即进行空闲判断 |
| Standby | 保留当前页面及内存；无任务时按 SDK 策略休眠；触摸唤醒可以作为独立选项 | 有输入或到期业务时执行相应工作；超时关机条件成立时进入 ScreenSaver |
| 锁屏 LOCKED | 保留原页面状态、阅读会话和输入草稿；显示一次锁屏画面后禁止普通 UI 更新；外设及后台服务按需求保留 | 有效解锁操作恢复原页面；超时关机或低电事件走各自路径 |
| ScreenSaver | 收拢业务、保存必要状态、显示一次关机画面，等待显示及面板断电完成 | 关机准备完成后进入 OFF |
| DeepSleep / OFF | 停止应用、网络和分页等业务，不依赖内存保留；屏幕靠墨水保持关机画面 | 合法开机按键触发重新启动；充电事件只进入关机充电管理，不启动正常应用 |
| LowBat | 有效电池数据确认低电后，在能量允许时显示一次低电图标，再进入受保护的关机状态；禁止按键开机 | 检测到充电后进入 Charge |
| Charge | 产品仍处于关机状态，仅允许必要的充电检测和保护；当前不实现专用充电动画 | 断开充电后按实际电量保持普通关机或 LowBat；达到开机阈值后只允许手动开机，不自动启动 |

无任务时立即判断能否休眠，不额外等待固定轮询周期。这里的“立即判断”与 SDK 的预计空闲时间门槛不同；设备仍在工作或下一项任务临近时，SDK 可以选择较浅模式。

无操作 5 分钟是自动关机策略的默认目标，后续接入现有超时关机设置。计时只由有效用户操作重置，天气同步、状态栏时间变化、分页完成和后台唤醒均不算用户操作。关机与锁屏是不同入口，不能每次锁屏都立即停止后台服务。

### 1.2 关机充电与低电恢复

“电量恢复”表示有效采样结果达到允许正常开机的阈值，不表示刚插入充电器。进入低电保护的阈值与允许重新开机的阈值应有间隔，并对采样进行稳定性判断，避免状态来回跳变。具体阈值依据电池、供电和测量电路确定。

充电插入、充电进行中以及达到开机阈值，都不自动进入主页、阅读页或正常后台服务。必要的硬件唤醒可以短暂执行充电检测后再次休眠；硬件被唤醒与产品自动开机是不同事件。专用充电画面及动画留作后续功能，本阶段不以动画或周期刷屏维持 Charge 状态。

LowBat 下禁止按键开机后，Charge 必须有重新判断电量并恢复合法按键开机条件的机制。优先采用充电芯片的状态事件；如果硬件不能报告足够的信息，再评估充电状态下有界、低频的测量唤醒。该测量不启动正常应用，也不延续天气/NTP 定时任务。实际事件源和测量方式需要板级电路确认。

### 1.3 产品状态与 SDK 模式的确定映射

| 产品行为 | 采用的 SDK 机制 | 恢复方式 |
|---|---|---|
| 运行中待机，包括停留在主页、阅读页时的空闲 | 以 `PM_SLEEP_MODE_DEEP` 为保留式休眠目标，由 RT-Thread/SiFli PM 根据任务和设备状态决定实际入睡深度 | 保持应用使用的 RAM/PSRAM 数据有效，唤醒后继续原线程，不重新运行应用初始化 |
| 锁屏期间无待处理任务 | 同样使用上述 DEEP 空闲策略；锁屏本身仍是 app_fwk 管理的页面状态 | 保留原页面、焦点和阅读会话；普通硬件唤醒不等于解锁，只有有效解锁操作才恢复原页面 |
| 产品 DeepSleep / OFF，即关机 | 完成项目关机事务后进入 `HAL_PMU_EnterHibernate()`；复用 `pm_shutdown()` 时保持 `BSP_PM_STANDBY_SHUTDOWN` 关闭 | 不依赖关机前内存内容，下次合法开机重新初始化并加载已持久化的业务数据 |

该映射是接入设计基线，具体板级供电、唤醒和恢复效果按第 7 节验证。DEEP 的实际进入仍受设备忙碌、禁止睡眠计数和最近任务到期时间约束，不由页面代码强制调用底层睡眠处理函数。

产品状态使用 `Standby` 表示运行中的待机，使用 `OFF` 明确标识产品关机；原产品状态名 `DeepSleep` 与 OFF 等价。SDK 的 `PM_SLEEP_MODE_STANDBY` 不作为产品 Standby 的直接映射，`PM_SLEEP_MODE_SHUTDOWN` 的空默认处理函数也不作为关机入口。

SDK 负责 CPU 休眠调度、时钟恢复、tick 补偿及最终 Hibernate；项目负责锁屏显示策略、业务收尾、外设供电编排、持久化和开机条件。Hibernate 不自动保存 RAM 镜像，也不替代这些项目职责。LowBat 和 Charge 继续遵循关机保护与充电不自动开机规则。

## 2. SDK 的能力与实际执行机制

### 2.1 运行模式和睡眠模式

SDK 分别管理运行频率与空闲休眠。降低运行频率不等同于进入 Deep Sleep；蓝牙协议连接的省电状态也不等同于 HCPU 已经休眠。

| 模式 | 本地实现的行为 | 接入定位 |
|---|---|---|
| `PM_SLEEP_MODE_IDLE` | CPU 等待事件，仍可保留高速时钟和工作的外设 | 外设忙碌期间允许的基础空闲状态 |
| `PM_SLEEP_MODE_LIGHT` | 关闭高速时钟，保存运行状态，唤醒后继续执行 | 浅睡验证或特殊场景 |
| `PM_SLEEP_MODE_DEEP` | 进一步控制电源和时钟；57 系列从 WFI 后继续，恢复时钟和板级状态 | 运行中、静态页面和锁屏期间采用的保留式休眠目标；不是产品 OFF |
| `PM_SLEEP_MODE_STANDBY` | 涉及上下文保存、启动恢复和设备恢复 | 后续独立评估内存布局及恢复代价 |
| Hibernate | 走关机及再次启动流程 | 产品 DeepSleep / OFF 采用的关机路径；业务状态从持久化存储重新加载 |

`PM_SLEEP_MODE_SHUTDOWN` 的默认 `sifli_shutdown_handler()` 为空。GUI PM 的 `pm_shutdown()` 默认调用 `HAL_PMU_EnterHibernate()`；启用 `BSP_PM_STANDBY_SHUTDOWN` 后走另一条状态处理路径。本方案采用默认 Hibernate 末端，项目关机准备必须在进入该末端之前完成。

### 2.2 入睡、唤醒与系统时间

实际执行顺序如下：

1. 所有更高优先级线程均无可运行工作时，RT-Thread 空闲线程进入 `rt_system_power_manager()`。
2. `_pm_enter_sleep()` 检查 `NONE/IDLE` 请求计数，并通过最近一个操作系统定时器的到期时间选择睡眠模式。
3. 注册的设备 suspend 回调检查是否可以休眠。SiFli 回调检查跨核消息是否为空以及已使能唤醒源是否正在触发。
4. `sifli_timer_start()` 停止 SysTick，使用低功耗定时器安排唤醒；HCPU 使用 `lptim1`。
5. `sifli_deep_handler()` 调用板级下电钩子、切换时钟、执行 WFI；醒来后恢复时钟和板级状态。
6. SDK 根据低功耗计时结果补偿 RT-Thread tick，处理到期定时器并恢复设备。

PM 回调在空闲线程的关中断路径内执行。阻塞式文件操作、等待互斥量、等待 DMA、面板放电延时和 LVGL 操作应在进入该路径之前由相应线程完成，不放入这些回调中。

HCPU 当前版本的实际策略表是：

| 编译选择 | 允许进入相应模式的最短预计空闲时间 |
|---|---:|
| `PM_DEEP_ENABLE` | 30 ms |
| `PM_LIGHT_ENABLE` | 100 ms |
| `PM_STANDBY_ENABLE` | 100 ms |

这些数值来自 `bf0_pm.c` 的 `pm_policy`，由 `low_power_init()` 注册并覆盖 RT-Thread 通用默认表。它们表示“距下一次定时任务到期还有多久”，不是“距离上次按键已经过去多久”。SDK 文档中的示意策略不能替代该版本实际表值。

当前 `RT_TICK_PER_SECOND=1000`。Deep Sleep 时停止周期性 SysTick，唤醒后再补偿；不需要通过停掉 RTC 或业务时间计算来延长睡眠。57 系列的定时器启动走 `rt_device_write()`，硬件定时器层在超出单次计数范围时可分段计时；30 分钟业务周期不保证硬件一次连续睡满 30 分钟。实际唤醒频度还受低频时钟、蓝牙和其他定时器影响。

### 2.3 禁止休眠与频率控制的职责

- `rt_pm_request(PM_SLEEP_MODE_IDLE)` 增加禁止进入更深休眠的计数，结束后由同一资源责任方 `rt_pm_release()`。该请求不是命令 CPU 立即进入某个模式。当前选择路径实际用 `NONE/IDLE` 计数阻止深睡，不应把其他模式的请求当作任意深度限制器。
- `rt_pm_hw_device_start()/stop()` 维护硬件忙碌计数，参与空闲降频判断。它与禁止深睡计数用途不同。
- `pm_scenario_start()/stop()` 按场景位选择运行档位，并不是禁止休眠锁。相同场景按位记录，不是可任意嵌套的引用计数。
- `gui_suspend()` 会调用 `pm_scenario_stop(PM_SCENARIO_UI)`，唤醒路径会调用对应 start。没有其他场景时，stop 请求 medium 档位。57 HCPU 的运行实现包含 240/144/48/24 MHz 档位，还会调整 Flash/PSRAM 时钟。

因此，采用 GUI PM 时就需要检查其场景降频行为。关闭 `BSP_PM_FREQ_SCALING` 只关闭相应的空闲降频路径，不能据此认定 `gui_suspend()` 不会改变运行频率。

### 2.4 Hibernate 入口与再次启动

本地 `HAL_PMU_EnterHibernate()` 在 HCPU 路径先关闭中断、唤醒并复位/停住 LCPU，清理 PMU 唤醒状态，再清除 `HIBER_EN`、等待 100 ms、设置 `HIBER_EN` 并等待关机。函数不返回原应用，文件保存和设备收尾必须在调用前完成。

`HAL_PMU_EnterShutdown()` 另外切换低频时钟、关闭 XTAL32，并通过 `RTC_ISR_INIT` 停止 RTC。为保留关机期间计时能力，本方案采用 SDK GUI PM 默认使用的 Hibernate 入口，不采用 `HAL_PMU_EnterShutdown()`；当前 `LXT_DISABLE=1`，RTC 在本板 Hibernate 期间能否持续正确计时仍需实测。关闭 RTC 的唤醒权限与停止 RTC 计时是两种不同操作。

SDK 启动阶段的 `rt_application_init_power_on_mode()` 调用 `HAL_PMU_CheckBootMode()`，将启动类型和唤醒源保存在 SDK 状态中。项目启动策略读取 `SystemPowerOnModeGet()` 及 HCPU 的 `pm_get_pwron_wakeup_src()`，区分按键、充电与其他原因；后者保存本次启动原因，而 `pm_get_wakeup_src()` 用于当前唤醒信息。底层检查会清除硬件状态，不应在项目中重复调用并期待再次读取同一现场。

已确认本地 `BSP_Hibernate_PowerDown()` 是空弱函数，上述 Hibernate HAL 入口没有调用它。实现同名函数本身不能完成外设关机，必须有实际调用路径。HAL 中关闭外围 LDO 的部分代码属于 52 系列条件分支，不能据此认定本板 57 的 TF、PSRAM 和外围电源会全部自动断开。

### 2.5 采用 SDK 例程的方式与边界

| 本地组件或例程 | 可复用的机制 | 本项目的适配边界 |
|---|---|---|
| `example/pm/gui_pm`，启用 LVGL V9 | GUI 活动状态、显示忙碌检查、GUI 挂起与事件唤醒 | 例程有 10 ms 循环和恢复后主动标脏，不直接照搬到墨水屏；不复制 52 系列引脚配置 |
| `example/pm/bt` 的 HCPU 例程 | 蓝牙回调向业务线程投递邮箱，主线程无事时阻塞等待 | 这是 BLE 示例，不是 PAN 功耗验证；57 配置关闭 MPI1/2，不能用于当前 PSRAM 布局 |
| `gui_app_pm` | 现有 GUI 状态机、信号量和 `pm_shutdown()` 的 Hibernate 末端 | 需适配触摸保留和显示 IdleMode；不能把原始每次按键直接映射到组件的休眠切换动作 |
| `sys_poweron_mng_init()` / `sys_poweron_fsm()` | SDK 的开机原因和事件组织可作为参考 | 当前实现对 RTC、部分充电唤醒调用 `sys_power_on()`，且含 30 秒等待；不直接采用其默认产品策略 |

项目侧只补充锁屏、关机准备和“充电不自动开机”等产品规则。CPU 休眠、tick 补偿、设备 PM 回调和最终 Hibernate 使用 SDK 的已有机制；页面栈仍由 app_fwk 独占管理。

## 3. 配置开关与依赖

| 宏 | 实际功能与依赖 | 接入安排 |
|---|---|---|
| `BSP_USING_PM` | 编入 SiFli PM，自动选择 `RT_USING_PM`；57 还会选择 `USING_CONTEXT_BACKUP` | 接入保留式休眠时启用，复核新增依赖的链接和内存占用 |
| `RT_USING_PM` | 空闲线程调用 PM；提供策略、设备回调和请求计数 | 由总开关选择 |
| `PM_DEEP_ENABLE` | 与 LIGHT、STANDBY 互斥；57 默认选择 DEEP | 保留式休眠阶段明确选择 |
| `GUI_APP_PM` | 编入 GUI 休眠状态机、信号量和显示设备控制 | 候选复用组件；需初始化并调用，不会自动接管主循环 |
| `GUI_APP_FRAMEWORK` | 应用、页面和导航生命周期 | 当前已接入；不等同于 GUI PM |
| `BSP_PM_FREQ_SCALING` | 空闲降频支持，Kconfig 为 HCPU 选择 BTIM1 | 核查资源和外设时钟后单独评估 |
| `BSP_PM_DEBUG` | 输出进入、退出和唤醒信息 | 接入阶段按需启用，正式电流测量时关闭频繁输出 |
| `PM_REQUEST_DEBUG` | 记录禁止休眠及硬件忙碌请求来源 | 用于定位计数不归零 |
| `PM_METRICS_ENABLED` | 软件统计睡眠时间、唤醒次数及来源 | 选择合适采集方式；不是电流测量 |
| `PM_METRICS_USE_COLLECTOR` / `PM_METRICS_PRINT_DIRECTLY` | 统计保存或定时打印，后者受 `PM_METRICS_PRINT_PERIOD` 控制 | 统计本身也会产生开销 |
| `BSP_PM_STANDBY_SHUTDOWN` | 让 GUI `pm_shutdown()` 走保留状态的 GUI 休眠路径 | 本方案 OFF 需要重新启动，保持关闭 |
| `BSP_PM_PIN_BACKUP_DISABLED` / `PM_ITCM_NOT_BACKUP` | 跳过特定状态保存恢复 | 初次接入保留默认，不作为通用节电开关 |
| `PM_USE_RC48` / `PM_LP_TIMER_DISABLE` / `PM_WAKEUP_PIN_AS_OUTPUT_IN_SLEEP` | Kconfig 限定在 LCPU 上使用 | 不加入当前 HCPU 初始配置 |
| `LXT_DISABLE` | 当前使用无外部低频晶振的配置路径 | 保留板级配置，先核实硬件再考虑变更 |
| `RT_USING_WDT` / `IWDT_SLEEP_TIMEOUT` | 看门狗及睡眠相关超时配置 | 如果启用看门狗，再核对唤醒与喂狗策略 |

运行期间保留式 DEEP 休眠的计划配置如下，仅用于后续接入，不代表已通过编译验证：

```text
CONFIG_BSP_USING_PM=y
CONFIG_PM_DEEP_ENABLE=y
```

选择 GUI PM 组件时再启用 `CONFIG_GUI_APP_PM=y`，并落实第 5.3 节的触摸策略。OFF 还需调用关机入口、配置允许的唤醒源并实现启动策略，不是多打开一个宏就完成。`USING_CONTEXT_BACKUP` 是 SDK 配置依赖，不意味着项目采用整块内存写入 Flash 的恢复方案。

现有 RTC、HWTIMER、LPTIM1 和 GPIO/按键能力可复用。配置生成后应检查最终 `rtconfig.h` 和链接 map，不只检查输入的 `proj.conf`。使用原板级构建目标；不以更换 SDK 或切换 LVGL 版本作为默认接入方式。

## 4. 当前工程的接入缺口

### 4.1 GUI 定时工作阻止达到深睡门槛

已确认主循环在每次执行 `ui_app_process()` 和 `lv_timer_handler()` 后最多等待 20 ms，小于 Deep Sleep 的 30 ms 门槛。当前 `LV_DEF_REFR_PERIOD=16`，app_fwk 调度定时器为该周期的两倍，即 32 ms。只修改主循环延时仍不充分，还要检查 LVGL 内部到期任务。

没有界面脏区只表示不需要重绘，不表示 CPU 没有周期醒来。静态页面应停止无必要的 GUI 周期执行，并通过输入、可见数据变化或确切到期时间恢复。

### 4.2 按键与唤醒配置

三个按键共用 PA34 中断入口，醒来后由 ADC 设备 `bat1` 的通道 6 区分键值。57 专用 HAL 将 PA33～PA42 映射为第一组唤醒脚，所以 PA34 的专用唤醒编号为 1，可通过 `HAL_HPAON_QueryWakeupPin()` 获取。

当前项目没有显式配置 PA34 专用深睡唤醒。SDK `init_default_wakeup_src()` 会使能 GPIO1、LPTIM1 和跨核唤醒源，所以“没有专用配置”不能直接证明所有 GPIO 唤醒路径均无效；接入时应明确注册实际按键源，并核对 PMUC 状态及真实唤醒行为。

PA24 是本项目面板 PWRCOM 控制脚，不能直接采用使用 PA24 的例程按键配置。按键唤醒模式需要结合实际电平、按下/释放中断和消抖验证；持续有效的电平可能使系统反复退出睡眠。

57 的 `pm_enable_pin_wakeup(1, mode)` 经 `HAL_HPAON_EnableWakeupSrc()` 调到 `HAL_PMU_EnablePinWakeup()`，实际配置 PMUC 的 PA34 唤醒位。此版本 `HAL_PMU_EnablePinWakeup2(pad, mode)` 仍直接返回 `HAL_ERROR`，应采用已经实现的编号接口，并检查返回结果。

普通 OFF 只保留合法开机按键和必要的充电检测唤醒；LowBat 禁止按键唤醒。PA34 是三个 ADC 按键的共用入口，硬件唤醒原因本身不能区分 KEY1/KEY2/KEY3。若后续限定某一按键开机，需要在启动早期恢复 ADC 后判别，并验证短按在启动前已经释放的情况。睡前按键释放、触发沿及开机后消费本次按键应统一处理，避免立即反复唤醒或把开机键再次当作页面确认。

### 4.3 显示完成与面板断电是两个完成条件

SDK LCD 任务已经在处理消息期间调用 PM request/release 和硬件忙碌计数，SDIO 命令传输也已有 PM request/release。应复用这些保护，不为同一传输再建立一套重复计数。

项目 EPD 在最后一帧完成中断中通知上层，并唤醒独立 `epd_pwr` 线程。最后一次刷新后空闲 300 ms，电源线程执行 `tps_enter_sleep()`：等待 10 ms、拉低 PWRCOM、等待 10 ms、拉低 PWRUP，再等待 200 ms。无新刷新时，从最后帧完成到函数结束包含约 520 ms 的配置等待，实际还取决于调度。这段等待不再阻塞上层获取刷新完成，但目前未独立接入 PM。

建议第一版由面板电源责任方保证供电转换、300 ms 合并窗口和放电完成前不会进入不适合的系统睡眠，完成后释放限制。错误和取消路径也必须配对释放。是否可在部分电源保持阶段提前让 HCPU 深睡，应由 GPIO 保持和电源时序测量决定，放在后续优化阶段。

### 4.4 外部内存入口存在实现限制

板级 `BSP_PowerDownCustom()/BSP_PowerUpCustom()` 默认为空，PSRAM 低功耗调用被注释。

进一步检查发现：当前通用 `bsp_psram_enter_low_power()` 的有效操作在 `BSP_USING_PSRAM0` 条件内，当前配置使用 `BSP_USING_PSRAM1`，未开启 PSRAM0；现有入口在该配置下返回成功但不执行那段硬件低功耗操作。`bsp_psram_deep_power_down()` 当前直接返回 -1。因此，恢复被注释的函数调用并不等于已经实现本板 PSRAM 省电。

锁屏及普通空闲休眠仍会使用 LVGL 堆、字形缓存、显示缓冲和灰度历史，必须保持数据有效。OFF 不再依赖这些内容，可在访问者停止后关闭相应供电，但实际断电能力仍取决于 PSRAM 型号、封装供电和板级电路。进入外部存储器低功耗或切断电源的末端代码、栈和常量不能依赖已经不可访问的存储器。是否需要最小 SDK 补充属于待确认项。

### 4.5 SDIO 与现有板级休眠接口

当前 TF 卡走 SDIO。`board_sleep_filesystem()` 等现有代码只在 SPI MSD 配置下控制卡电源，不能覆盖本板。SDK SDIO 传输保护可以复用，但它不等于卡电源管理。

锁屏阶段优先保持卡供电，验证传输结束后 HCPU 可以休眠。OFF 则在文件写入完成、打开的文件和传输收尾后关闭可控的卡电源，下次启动重新初始化和挂载。实际电源开关是否存在需要板级确认；SDK 的 SDIO suspend 不等于 TF 已经断电。

### 4.6 业务任务与定时工作

| 当前工作 | 已确认的执行方式 | 建议 |
|---|---|---|
| 天气后台 | 每秒等待超时后检查网络；固定 30 分钟同步；失败后受控 5 秒重试 | 网络变化事件加最近到期时间等待，保留原周期和重试语义 |
| 天气 UI | 前台每 250 ms 检查快照/操作结果 | 数据版本或请求完成通知 UI，休眠时只保存最新状态 |
| 状态栏 | 每秒检查日期时间、电量和无线状态；时间显示到分钟 | 时间按分钟边界处理，其他状态按事件或各自采样周期处理 |
| NTP | 本次连接同步成功或尝试结束后阻塞，连接变化时唤醒 | 保留，不重复改造成轮询任务 |
| 阅读分页 | 在 UI 线程分步调用 LVGL 字体度量，离开阅读页仍可继续索引 | 第一版在分页未完成时延后 GUI 挂起；避免突然停住任务或并发访问 LVGL |
| 手动同步弹窗 | 成功提示在显示完成后保持规定时间，再关闭 | 将提示收尾纳入到期工作，不能因 GUI 挂起而无限停留 |
| 蓝牙关闭 | 停扫描、停重连、断开链路，未明确关闭控制器 | 后续核查正式控制器接口和 LCPU 休眠，不把 UI 的 OFF 当成物理断电 |
| 超时关机设置 | 当前只有选项值变化，没有关机计时执行 | 接到 ScreenSaver → OFF 事务，与普通保留式休眠分开实现 |

### 4.7 锁屏当前仍可能更新状态栏

`page_create()` 为锁屏保留公共页头，只隐藏电池显示。`launcher_set_status()` 对当前前台页面调用 `status_render()`，没有排除锁屏；`ui_app_process()` 又每秒调用状态检查。因此当前“进入锁屏页”还不等于“锁屏后不再刷新”。

后续应继续接收并保存最新业务快照，但锁屏期间不改状态栏、天气摘要等控件，不打开后台结果弹窗，也不因这些结果解锁。解锁后由 UI 线程统一读取最新快照并恢复原页面。低电关机等系统级转换是单独的明确事件，不受普通 UI 冻结限制。

### 4.8 电池与充电检测尚不具备闭环条件

当前 `battery.c` 的 SF32LB57 分支为占位实现：电压返回 0、百分比返回 0、充电状态返回 false。UI 将未知电量显示为 0% 可以保留，但该值不能触发 LowBat。

接入前需要有效性明确的电压/电量结果、充电状态和采样时间，以及真实的电池与充电引脚映射。57 PMU 头文件虽提供 `HAL_PMU_EnableChgWakeup()`，寄存器也有 `PMUC_WER_CHG`，这些源码只能证明接口存在，不能证明本板充电器已连接该检测通道。若实际使用外接充电芯片的状态 GPIO，必须依据原理图选择对应的常供电唤醒输入。

## 5. 推荐的集成结构

### 5.1 组件所有权

| 组件 | 负责的状态和资源 |
|---|---|
| RT-Thread PM + SiFli PM | 入睡策略、tickless、时钟和处理器状态、设备恢复 |
| `gui_app_pm`（按需接入） | GUI 活动状态、挂起信号量、显示 IdleMode、运行场景接口；使用前满足触摸策略 |
| `app_fwk` | 唯一的页面栈、screen、页面上下文和导航生命周期 |
| 项目侧 PM 协调层 | 锁屏/关机产品规则、工作收尾、合法开机判断、GUI 线程交接和组件编排 |
| EPD 电源线程 | 面板上下电、连续刷新合并窗口和断电完成 |
| 各业务服务 | 天气/PAN/NTP/文件任务及持久化，不依赖页面存续 |

项目侧协调层只保存必要的产品状态和关机准备进度，不再实现一套 CPU 睡眠调度或页面栈。普通静态页面睡眠保留当前 screen、焦点和输入草稿，恢复时比较最新快照，只更新改变的内容。

### 5.2 GUI PM 与页面生命周期的边界

`gui_app_fwk_suspend()` 通过邮箱发送 `GUI_APP_MSG_SUSPEND_SCHEDULER`，实际处理会把页面目标状态设为 paused；恢复调度会恢复最后页面。这不是单纯停止一个定时器。

当前阅读页 ONPAUSE 会保存位置、取消待翻页并请求下一次全刷，ONRESUME 也会请求全刷。因此，把每次普通空闲休眠映射成 app_fwk 暂停/恢复，会改变翻页策略，甚至使每次唤醒后的刷新都变成全刷。

建议第一版普通空闲休眠使用 GUI 线程挂起、保留 app_fwk 的页面状态，不发起页面暂停/恢复导航消息。停止 GUI 线程执行后，框架的 LVGL 调度也不再周期运行。真正离开页面仍走现有生命周期；后续只有确实需要页面获得系统挂起通知时，才另外定义 PM 通知职责。

### 5.3 显示设备使用 IdleMode

`gui_pm_init()` 默认使用显示 POWEROFF/POWERON。该路径重新打开设备时会调用 EPD `LCD_Init()`，使灰度缓存无效并重新初始化面板。例程还会在恢复后主动重绘，不适合直接套用。

候选方案是初始化后设置 `gui_set_idle_mode(true)`。SDK 此时调用 `RTGRAPHIC_CTRL_SET_MODE`，当前 EPD 的 `IdleModeOn()` 关闭面板电源，`IdleModeOff()` 不主动上电，由下一次真实刷新负责开启。这条路径不重新调用 `LCD_Init()`，更适合保持局刷历史。

采用前还需验证 LCD 消息已经处理完成、GPU/LCDC 时钟恢复正确、框架 screen 未被无条件标脏，以及无新内容时不会产生额外刷新。

另一个明确限制是：本地 `close_display()` 无论是否选择 IdleMode，都会对找到的 `touch` 设备发送 POWEROFF；`open_display()` 再发送 POWERON。当前没有独立的“睡眠时保留触摸”策略参数。若产品启用触摸唤醒，不能直接套用这条流程。可以先在项目侧使用事件等待和现有 EPD 电源线程，继续复用 RT-Thread/SiFli PM；需要复用完整 GUI PM 时，再评估最小的触摸策略接口扩展。不能以静默关闭触摸替代已启用的唤醒功能。

### 5.4 休眠与唤醒的线程交接

`gui_pm_fsm()` 会同步调用注册的 handler，回调运行在调用者线程并处于 GUI PM 互斥保护内。`gui_suspend()` 会阻塞调用线程；真正唤醒后，该线程继续执行。SDK 已有信号量可用于这次等待。

建议实施顺序：

1. UI 线程完成输入批次、导航及显示收尾。存在待翻页、分页、必要提示计时或其他 GUI 工作时，暂不进入 GUI 挂起。
2. 进入 GUI 休眠准备状态后再次检查输入队列和待处理事件；有新工作则取消本次挂起。不能只在进入准备状态前检查一次。
3. 在 UI 线程停止 LVGL 周期处理，并按组件顺序进入显示 IdleMode 和阻塞等待。阻塞期间不持有业务快照锁、页面锁或面板电源互斥量。
4. GPIO 中断只完成硬件事件交接；按键库线程按原规则入队，释放队列临界区后通知 GUI PM 唤醒。方向合并提前返回的路径也必须完成唤醒通知。
5. GUI PM 的外线程回调只发布恢复状态，不操作 LVGL 控件、不递归调用同一状态机。UI 线程返回后恢复 LVGL 执行，读取最新数据，再按原顺序消费输入。

入队与休眠存在竞态：如果输入在“检查队列为空”之后、真正等待之前到来，也必须能取消挂起或留下唤醒信号。验证需覆盖 ACTIVE、INACTIVE_PENDING、INACTIVE 三种时机。硬件唤醒成功与 GUI 线程已经被唤醒是两个独立条件。

后台任务唤醒 HCPU 不一定要唤醒 GUI。天气数据不可见时只更新缓存和版本；数据在当前页面可见时合并成一次界面处理。时间边界、弹窗关闭等需要 GUI 的到期事件应明确唤醒 GUI，而不是依赖已经停掉的主循环。

### 5.5 板级恢复与内存

Deep Sleep 的 57 路径会执行 `BSP_IO_Power_Down(CORE_ID_HCPU, false)` 和 `BSP_Power_Up(false)`。钩子的布尔参数不能按名称误读为“false 表示不是 Deep Sleep”；项目实现应根据真实调用路径定义职责。

保留式休眠初次接入时保持内存和外设供电策略尽量不变，先建立完整入睡/恢复路径。后续外部内存省电需确认保留模式、执行代码位置、缓存维护和恢复顺序。SDK Standby 涉及上下文备份、保留区与自定义链接脚本，不是本方案实现 OFF 的必要步骤。

### 5.6 锁屏与解锁流程

1. UI 线程按现有输入顺序处理锁屏导航，由 app_fwk 保存和管理原页面。阅读位置、焦点、书架列表位置和城市输入草稿继续使用现有保存职责，不复制一套导航栈。
2. 锁屏页面完成一次绘制和发送后，冻结普通可见 UI 更新；面板电源仍由 `epd_pwr` 管理。期间收到的后台数据只更新模型。
3. 无 GUI 工作时阻塞等待事件。后台分页如果仍在 UI 线程执行 LVGL 字体度量，就先完成必要计算或保留有界计算调度；不能为了停止刷屏而让计算永久停止。保留内存及服务并不要求反复绘制。
4. 解锁操作回到原页面，恢复原焦点、阅读位置和草稿，并比较最新数据。真正的锁屏导航可以触发现有页面暂停/恢复；普通 CPU 休眠则不触发页面生命周期。
5. 保留现有输入合并、确认/返回顺序、分页完成后仅更新一次底栏、局刷跳过未变化像素及全刷周期规则。解锁后的实际页面恢复沿用既有刷新策略，不因 GUI PM 唤醒额外增加全刷。

### 5.7 关机事务

关机由可运行线程完成准备，最后才进入 SDK 的不返回入口。任务先停止产生新工作，再等待现有工作到达安全结束点。

| 顺序 | 工作与完成条件 |
|---|---|
| 1. 进入关机准备 | 停止接收新的翻页、导航、扫描和同步请求；记录关机原因。关机提交点之后的按键不能再落入原页面事件队列 |
| 2. 收拢后台服务 | 停止天气周期/重试、PAN 自动重连和 NTP 新任务；让已发出的 HTTP、文件读取、分页工作完成或按既有取消能力有界退出。等待时不持有业务锁 |
| 3. 保存必要状态 | 保存阅读字节位置、已提交设置和必要恢复信息，核对写入结果。天气由原服务提交最新有效缓存；不为关机额外等待一次联网成功 |
| 4. 输出最终画面 | 正常关机显示一次关机画面；低电关机在剩余能量允许时显示一次低电图标。通过 UI 线程和现有显示路径绘制，不在 PM 设备回调中操作 LVGL |
| 5. 等待两类完成 | 先确认最后一帧传输完成，再确认面板电源线程完成放电并关闭供电。复用电源责任方的完成事件或状态查询，不把“上层收到刷新完成”或固定延时当作断电完成 |
| 6. 关闭可控外设 | 确认文件同步、文件关闭及 SDIO/DMA 等传输完成，再停用外设和板级可控电源；PSRAM/Flash 失去访问能力前，所有使用者和执行依赖必须已退出 |
| 7. 配置下次启动条件 | 按正常 OFF、LowBat、Charge 的规则配置按键和充电唤醒，移除不属于关机策略的 RTC/触摸等唤醒；处理仍被按住的按键 |
| 8. 进入 Hibernate | 准备完成后使用 `pm_shutdown()` 默认末端或 `HAL_PMU_EnterHibernate()`；不在关中断之后等待信号量、写文件或等待业务线程 |

准备阶段需要区分正常关机与低电保护：正常关机的重要状态保存失败时，不宣告保存成功，应按明确的失败策略保留运行或取消关机；低电保护不能无限重试写入或强行刷屏耗尽剩余电量，必要时保留上一次有效记录并优先保护电源。具体超时和电压门槛在硬件验证后确定。

关机收尾只需要停止访问并完成写入，不必为了即将丢弃的 RAM 再逐一释放全部堆对象。若准备失败需要返回运行，必须能够解除已经设置的业务门禁；只有在不可逆断电之前完成该选择。

### 5.8 持久化与重建边界

| 数据或资源 | 锁屏 | OFF 与下次开机 |
|---|---|---|
| 阅读位置 | 保留当前会话，沿用现有字节偏移保存 | 保存书籍标识和源文件字节位置；重新打开文件后按实际字体和排版定位，页码不是唯一锚点 |
| 分页索引 | 后台可继续计算，结果不推动锁屏重绘 | 复用现有完整索引缓存；文件或排版条件变化则重新分页，不强行保存未完成索引为完整结果 |
| 书架选择、原页面 | 由页面上下文和现有保存变量恢复 | 如需跨关机恢复，保存稳定的书籍/页面标识和必要位置；不序列化 app_fwk 栈内存或控件指针 |
| 设置与天气 | 保留内存并按原规则持久化 | 复用 `ui_settings_store`、`weather_store`，加载最近一次有效记录 |
| 未提交输入 | 保留城市 ID 等现有草稿 | 不作为已提交设置写入；跨关机恢复草稿不是本阶段默认能力 |
| TF 字体、字形缓存 | 保留数据及有效引用 | 重新从 TF/Flash 加载字体，重新建立缓存；TF 缺失或字体不可用时回退内置字体，并重新验证排版索引 |
| LVGL/app_fwk/RTOS/网络 | 保留运行现场 | 从初始化流程重建；不恢复指针、句柄、锁、TCP/TLS 会话或 DMA 状态 |
| 墨水屏新旧灰度历史 | 保留，用于现有局刷策略 | RAM 历史作废；首次真正显示正常界面走全刷，建立新的可信历史后再局刷 |

现有设置存储采用两个 Flash 扇区轮换，天气存储采用多槽轮换；两者都有校验、版本、序号和最后提交标记。阅读位置也已有轮换文件及 `fsync()`。后续复用这些实现，不为关机再引入一套通用快照文件系统。

`ui_reader_save()` 当前返回 void，关机协调不能仅凭调用完成就认为保存成功；应把原有位置存储的成功状态纳入关机完成判断。分页 `.idx` 是可重建缓存，保存失败不应与丢失最新阅读位置同等处理。未来新增跨关机页面恢复记录时，同样需要版本、有效性检查及完整写入后提交。

TF 字体全量加载到 PSRAM 后会在关机时丢失，开机重新读入即可；若字体已有有效的 NOR 副本，复用该副本。无需在每次关机时把字体和整块 PSRAM 再复制到 Flash。

### 5.9 重新开机顺序

1. 完成 SDK 必需的板级初始化后，尽早读取 SDK 保存的启动类型和唤醒源，并采集有效电池/充电状态。正常 PAN、天气、扫描和完整 UI 启动放在开机条件通过之后；核查自动初始化项，避免它们先行启动业务。
2. 充电原因唤醒只执行关机充电管理，不能调用默认开机策略进入正常页面。LowBat 条件未解除时不接受按键开机；阈值满足也只开放手动开机条件。
3. 合法手动开机时初始化存储，载入已提交设置、阅读记录和天气缓存；初始化 LVGL 及字体组件后加载所需字体，再由 app_fwk 创建页面对象。缺少 TF 或文件时回退到可用页面，不等待不存在的资源。
4. 如需恢复阅读，先建立实际字体和布局，再根据源文件字节位置定位；只在布局和文件检查通过时复用分页索引。恢复对象来自本次新建，不来自关机前内存。
5. 通过现有显示驱动全刷首个正常界面，建立灰度历史；随后按既有规则重连 PAN，网络就绪后同步时间和天气。RTC 无有效时间时沿用未知时间显示，不能从旧 RT-Thread tick 推算关机时长。
6. 结束开机按键的消费，再开放正常输入队列，避免开机动作同时触发翻页或页面确认。

## 6. 实施阶段与退出条件

| 顺序 | 工作内容 | 完成标准 |
|---|---|---|
| 1 | 锁屏冻结可见更新，保留现有业务和页面状态 | 锁屏完成后面板不再刷新；后台同步照常；解锁恢复页面、焦点和最新数据 |
| 2 | 启用保留式 DEEP、配置 PA34、接入事件等待和面板断电协调 | 静态页面能休眠和唤醒；无额外全刷、输入丢失或残影回归；触摸策略符合设置 |
| 3 | 天气/状态通知及精确到期调度 | 减少轮询唤醒；30 分钟同步、5 秒重试、NTP 和分页结果行为保持正确 |
| 4 | 普通关机事务、Hibernate 与手动冷启动 | 写入和面板断电完成后关机；清空运行内存仍能恢复必要业务数据；首屏全刷后局刷正常 |
| 5 | 真实电池检测、充电唤醒、LowBat / Charge 与超时关机 | 有效测量驱动保护；未知电量不误关机；按键限制正确；充电及电量恢复均不自动开机 |
| 6 | PSRAM/TF 实际断电、触摸、音频、LED、GPIO 和蓝牙省电 | 每项分别测量收益和恢复正确性；闭环确认 OFF 外设电源轨及锁屏联网电流 |

上述次序按接入依赖安排，并非实测功耗排名。板级电源和唤醒能力应在第 4 阶段前完成核查，才能确定实际接入范围；在外设电源轨尚未验证之前，只能声称关机流程成立，不能宣称已经达到目标关机功耗。PSRAM、TF 卡和无线各自占多少电流仍需测量。

## 7. 验证方案与待确认事项

### 7.1 软件与硬件分别验证

- 编译检查：使用当前板级配置，检查最终 PM 宏、上下文备份依赖、链接段和内存占用；不把编译成功等同于运行恢复正确。
- 软件检查：使用 SDK 已有 `pm_dump`、PM 调试和按需统计，核查入睡模式、禁止睡眠计数、唤醒来源以及实际睡眠时长。
- 电流检查：记录测量位置是电池输入还是芯片/外设电源轨、供电电压、USB/调试器连接状态及测量窗口。软件报告休眠只证明执行了相关路径，不能替代电流测量。
- 对比窗口：同一硬件、页面、蓝牙状态、TF 卡和测量时长，分别记录静态阅读、锁屏、PAN 在线空闲、翻页及同步操作。平均电流为窗口内电流积分除以时长；操作比较还需包含唤醒、处理和重新入睡的完整周期。

### 7.2 上板验收矩阵

| 场景 | 核查内容 |
|---|---|
| 静态主页/阅读 | 按策略入睡，面板已断电；无内容变化时不刷屏 |
| SDK 模式映射 | 普通 DEEP 唤醒继续原应用，不重复初始化；OFF 后由合法开机原因触发新一轮初始化，业务状态来自持久化存储 |
| 锁屏及后台数据变化 | 锁屏完成后时间、天气、无线变化均不刷屏；解锁后恢复原页面并显示最新快照 |
| 三个按键唤醒 | 冷闲置后均能识别；短按、长按、释放无重复操作 |
| 休眠边界连续输入 | 按键在准备休眠和正式等待之间到来仍能处理；方向合并和确认/返回顺序不变 |
| 连续翻页后空闲 | 最终目标正确，300 ms 合并窗口与放电流程结束后允许深睡；没有睡眠后的错误全刷 |
| 锁屏后页面恢复 | 原焦点、书架位置、城市输入草稿、阅读菜单和书籍字节位置保留 |
| 分页未完成 | 工作按选定策略继续，完成后只更新一次总页数底栏 |
| 天气和 NTP | PAN 就绪、断线重连、30 分钟到期、5 秒重试、手动弹窗收尾均正常；后台线程不操作控件 |
| SDIO 与持久化 | 休眠不打断传输；读书、保存位置和天气缓存无损坏；若实现卡掉电，覆盖重新初始化 |
| 内存恢复 | 字形、图片、页面对象和新旧灰度缓冲有效，连续睡醒不出现花屏、黑块或残影 |
| 长时间联网待机 | 区分 LPTIM 分段、蓝牙事件和业务定时造成的唤醒，检查 RTC/tick 时间一致性 |
| 关机完成边界 | 示波器对应最后帧结束、PWRCOM/PWRUP 和系统低功耗进入时刻；确认先完成面板放电，再进入 OFF |
| 无 RAM 保留的重启 | 丢弃全部运行对象后启动；设置、阅读字节位置、天气缓存可恢复；缺卡、缺字体或索引过期有正常回退 |
| OFF 首屏与后续局刷 | 首次显示全刷建立新灰度历史；之后无旧缓存造成的花屏、残影或错误跳过像素 |
| 写入期间关机和异常中断 | 写入完成才报告成功；下次加载最新完整或上一份有效记录，不能加载半写记录 |
| 开机关机边界按键 | 按住、短按后释放、多键共用 PA34 均有明确结果；不自动再次关机或重复触发页面操作 |
| 低电与充电 | 未知数据不触发保护；有效低电只显示一次；LowBat 禁止按键开机；插拔充电和达到阈值均不自动进入应用 |
| 自动关机计时 | 仅用户操作重置超时；后台天气/NTP/分页不会无限延长关机时间；收尾未完成不提前断电 |

### 7.3 待确认清单

1. PA34 专用唤醒的触发模式、有效电平和 ADC 采样恢复是否在实际板上可靠。
2. 面板 IdleMode 的异步消息完成边界，以及 HCPU 深睡期间控制脚保持情况。
3. 当前 PSRAM 型号对应的有效保留低功耗指令及安全调用位置；现有通用入口不能直接证明支持。
4. GUI 场景升降频对当前 MPI、SDIO、GPU 和 EPD 时钟的影响；是否需在项目侧约束场景策略。
5. 蓝牙控制器允许休眠的实际状态和 PAN 保持连接的电流；`bt_sleep_control()` 的存在不证明它已被正确使用。
6. 长时间休眠中的低功耗定时器分段唤醒及 RC 时钟精度；必要时再评估 RTC 定时源。
7. 自定义内存布局在启用 `USING_CONTEXT_BACKUP` 后的构建兼容性，以及保留式休眠需要的代码和数据放置。
8. 电池采样、充电器类型、充电事件实际连线，以及允许正常开机/低电保护的电压阈值和稳定时间。
9. TF、PSRAM、触摸等电源轨是否独立可控；GPIO 是否可能反向供电；执行 Hibernate 末端时的内部 RAM 代码、栈及常量是否完整。
10. Hibernate 期间 RTC 的实际连续性及启动原因保留；Charge 的状态检测方式能否在不启动正常应用的前提下完成。

## 8. 源码索引

以下链接定位本地研究基线；更新源码后以符号名称和条件编译分支重新核对，不仅依赖行号。

| 内容 | 位置与主要符号 |
|---|---|
| 实际构建配置 | [rtconfig.h](/D:/program/LV_EpdReder/project/build_dpi-hdk_lb57gyd7n6_epd_hcpu/rtconfig.h)、[.config](/D:/program/LV_EpdReder/project/build_dpi-hdk_lb57gyd7n6_epd_hcpu/.config:112) |
| PM Kconfig | [middleware/system/Kconfig](/D:/program/LV_EpdReder/SiFli-SDK/middleware/system/Kconfig:1)、[drivers/Kconfig](/D:/program/LV_EpdReder/SiFli-SDK/rtos/rtthread/components/drivers/Kconfig:138) |
| PM 策略与底层流程 | [bf0_pm.c](/D:/program/LV_EpdReder/SiFli-SDK/middleware/system/bf0_pm.c:305)：`pm_policy`、`sifli_deep_handler`、`sifli_timer_start`、`low_power_init`、`pm_scenario_start/stop` |
| DEEP 保留式恢复 | [sifli_deep_handler](/D:/program/LV_EpdReder/SiFli-SDK/middleware/system/bf0_pm.c:1022)：SF32LB57 从 WFI 后继续执行，恢复时钟和板级状态 |
| RT-Thread 调度与计数 | [pm.c](/D:/program/LV_EpdReder/SiFli-SDK/rtos/rtthread/components/drivers/pm/pm.c:411)：`_pm_enter_sleep`、`rt_pm_request/release`、`rt_pm_run_enter` |
| 空闲等待 | [idle.c](/D:/program/LV_EpdReder/SiFli-SDK/rtos/rtthread/src/idle.c:225)、[context_gcc.S](/D:/program/LV_EpdReder/SiFli-SDK/rtos/rtthread/libcpu/arm/cortex-m33/context_gcc.S:244) |
| GUI 电源组件 | [gui_app_pm.c](/D:/program/LV_EpdReder/SiFli-SDK/middleware/system/gui_app_pm.c:302)：`gui_suspend`、`gui_pm_fsm`、`gui_set_idle_mode`、`pm_shutdown` |
| V9 GUI PM 例程 | [gui_pm/src/main.c](/D:/program/LV_EpdReder/SiFli-SDK/example/pm/gui_pm/src/main.c:131)、[proj.conf](/D:/program/LV_EpdReder/SiFli-SDK/example/pm/gui_pm/project/proj.conf)：配置启用 V9；例程验证板为 52 系列，不作为 57 板硬件验证结果 |
| 带 57 分支的蓝牙 PM 例程 | [bt/src/hcpu/main.c](/D:/program/LV_EpdReder/SiFli-SDK/example/pm/bt/src/hcpu/main.c)：`app_wakeup`；[57 配置](/D:/program/LV_EpdReder/SiFli-SDK/example/pm/bt/project/hcpu/sf32lb57x/proj.conf) 关闭 MPI1/2，不能照搬到依赖 PSRAM 的项目 |
| 框架调度暂停 | [gui_app_fwk.c](/D:/program/LV_EpdReder/SiFli-SDK/middleware/app_fwk/gui_app_fwk.c:1196)、[app_schedule.c](/D:/program/LV_EpdReder/SiFli-SDK/middleware/app_fwk/app_schedule.c:2376) |
| 主循环和输入 | [main.c](/D:/program/LV_EpdReder/src/main.c:93)、[ui_app.c](/D:/program/LV_EpdReder/src/ui/ui_app.c:17)、[buttons.c](/D:/program/LV_EpdReder/src/boards/controls/buttons.c:141) |
| 页面与阅读任务 | [launcher.c](/D:/program/LV_EpdReder/src/ui/launcher.c:1061)：`page_lifecycle`、`launcher_process`；[ui_reader.c](/D:/program/LV_EpdReder/src/ui/ui_reader.c:329)：`ui_reader_process` |
| EPD 电源及灰度缓存 | [epd_e0470a03.c](/D:/program/LV_EpdReder/SiFli-SDK/customer/peripherals/display/epd_e0470a03/epd_e0470a03.c:484)、[epd_tps.c](/D:/program/LV_EpdReder/SiFli-SDK/customer/peripherals/display/epd_e0470a03/epd_tps.c:189) |
| LCD 与 SDIO 传输保护 | [drv_lcd.c](/D:/program/LV_EpdReder/SiFli-SDK/rtos/rtthread/bsp/sifli/drivers/drv_lcd.c:2067)、[drv_sdio.c](/D:/program/LV_EpdReder/SiFli-SDK/rtos/rtthread/bsp/sifli/drivers/drv_sdio.c:574) |
| 57 唤醒引脚 | [bf0_hal_hpaon_sf32lb57x.c](/D:/program/LV_EpdReder/SiFli-SDK/drivers/hal/bf0_hal_hpaon_sf32lb57x.c:19)、[bf0_hal_aon_sf32lb57x.h](/D:/program/LV_EpdReder/SiFli-SDK/drivers/Include/bf0_hal_aon_sf32lb57x.h:26) |
| 板级电源和 PSRAM | [bsp_power.c](/D:/program/LV_EpdReder/SiFli-SDK/customer/boards/sf32lb57-dpi-hdk_base/bsp_power.c:25)、[flash.c](/D:/program/LV_EpdReder/SiFli-SDK/customer/boards/common/flash.c:1202) |
| 项目板级接口 | [board_service.c](/D:/program/LV_EpdReder/src/boards/board_service.c:172)：`board_sleep_filesystem`、`board_prepare_to_sleep` |
| 后台服务 | [weather.c](/D:/program/LV_EpdReder/src/services/weather/weather.c)、[network_time.c](/D:/program/LV_EpdReder/src/services/net/network_time.c)、[bt_pan.c](/D:/program/LV_EpdReder/src/services/net/bt_pan.c) |
| 蓝牙核休眠控制 | [bf0_bt_common.c](/D:/program/LV_EpdReder/SiFli-SDK/middleware/bluetooth/service/common/bf0_bt_common.c:741)：`bt_sleep_control` |
| 长超时分段 | [hwtimer.c](/D:/program/LV_EpdReder/SiFli-SDK/rtos/rtthread/components/drivers/hwtimer/hwtimer.c:14)：`timeout_calc`、`rt_hwtimer_write` |
| Hibernate 与 Shutdown 差异 | [bf0_hal_pmu.c](/D:/program/LV_EpdReder/SiFli-SDK/drivers/hal/bf0_hal_pmu.c:209)：`HAL_PMU_EnterHibernate`、`HAL_PMU_EnterShutdown`、`HAL_PMU_CheckBootMode` |
| 启动类型与唤醒现场 | [bf0_pm.c](/D:/program/LV_EpdReder/SiFli-SDK/middleware/system/bf0_pm.c:612)：`rt_application_init_power_on_mode`；[启动原因查询](/D:/program/LV_EpdReder/SiFli-SDK/middleware/system/bf0_pm.c:910)：`pm_get_pwron_wakeup_src` |
| 默认开机策略 | [gui_app_pm.c](/D:/program/LV_EpdReder/SiFli-SDK/middleware/system/gui_app_pm.c:464)：RTC/充电分支、30 秒等待和 `sys_poweron_fsm` |
| 57 PMU 引脚接口实现 | [bf0_hal_pmu_sf32lb57x.c](/D:/program/LV_EpdReder/SiFli-SDK/drivers/hal/bf0_hal_pmu_sf32lb57x.c:176)：编号接口有效，`EnablePinWakeup2` 返回错误；[HCPU 包装接口](/D:/program/LV_EpdReder/SiFli-SDK/middleware/system/bf0_pm.c:2707) |
| 充电唤醒能力定义 | [bf0_hal_pmu.h](/D:/program/LV_EpdReder/SiFli-SDK/drivers/Include/bf0_hal_pmu.h:644)、[57 PMUC 寄存器](/D:/program/LV_EpdReder/SiFli-SDK/drivers/cmsis/sf32lb57x/pmuc.h:134) |
| GUI PM 触摸电源行为 | [gui_app_pm.c](/D:/program/LV_EpdReder/SiFli-SDK/middleware/system/gui_app_pm.c:137)：`close_display`、`open_display` |
| 锁屏仍更新状态栏的位置 | [launcher.c](/D:/program/LV_EpdReder/src/ui/launcher.c:306)：`launcher_set_status`；[锁屏页创建](/D:/program/LV_EpdReder/src/ui/launcher.c:984)；[状态轮询](/D:/program/LV_EpdReder/src/ui/ui_app.c:110) |
| 电池检测占位分支 | [battery.c](/D:/program/LV_EpdReder/src/boards/battery/battery.c:105)：SF32LB57 返回固定值，不能作为 LowBat 的依据 |
| 业务数据持久化 | [ui_settings_store.c](/D:/program/LV_EpdReder/src/ui/ui_settings_store.c:63)、[weather_store.c](/D:/program/LV_EpdReder/src/services/weather/weather_store.c:62)、[ui_bookshelf_data.c](/D:/program/LV_EpdReder/src/ui/ui_bookshelf_data.c:113) |
| 阅读索引与位置 | [ui_reader.c](/D:/program/LV_EpdReder/src/ui/ui_reader.c:147)：`index_save`、`position_save`；[保存入口](/D:/program/LV_EpdReder/src/ui/ui_reader.c:373)：`ui_reader_save` |
| 末端代码的链接放置 | [link.lds](/D:/program/LV_EpdReder/project/dpi-hdk_lb57gyd7n6_epd_hcpu/link.lds:143)：内部 RAM 的 `.retm_data` 规则；最终启用配置后以 map 核查调用链 |
