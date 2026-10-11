# 动态应用故障取证与手工定位

## 构建与保存符号

平台及应用沿用原来的构建入口。平台默认启用 `RT_USING_MODULE_DIAG`；该开关只为 SDK 模块对象增加构建身份和装卸日志，不增加诊断线程。关闭该开关后，模块仍能按原方式运行。

```powershell
. .\SiFli-SDK\export.ps1
scons -C project --board=dpi-hdk_lb57gyd7n6_epd_hcpu APPS=weather,books -j8
```

安装目录仍为 `project/build_<board>/app-resources/<应用ID>/`。符号保存在同一构建目录的 `debug-artifacts/symbols/<GNU-build-id>/`，对应关系集中在 `debug-artifacts/manifest.json`。平台独立构建不依赖应用，单独构建应用只归档所选应用及私有库。旧构建身份的符号不会被同名新模块覆盖。

`.so.nostrip` 是唯一链接结果；安装用 `.so` 从它复制并裁剪，保留动态符号、重定位和 GNU build-id，不包含完整调试信息。GNU build-id 用于找符号，`app.json` 中的平台兼容标识仍用于判断 ABI 是否匹配，两个标识不能互换。

SDK 模块结构变化后，固件和待部署应用应重新构建并配套更新。没有 build-id 的旧模块仍可以被 SDK 加载，但无法凭模块名确认精确符号。

交付一次可复现的调试输入时，显式导出：

```powershell
python tools/debug_artifacts.py --build project/build_dpi-hdk_lb57gyd7n6_epd_hcpu --output project/build_dpi-hdk_lb57gyd7n6_epd_hcpu/debug-bundle.zip
```

调试包包含所选固件、已归档应用和私有库的符号、构建配置、Git 提交、源码差异及未跟踪输入。普通构建不导出完整源码或调试 ZIP。接收者先取得清单指定的项目、SDK 及子模块提交，再应用各仓库的 `changes.patch`、恢复对应 `untracked/` 内容。构建依赖的外部工具仍按清单中的版本准备。

源码已改变时，导出会拒绝把当前源码冒充旧构建源码。请在同一源码状态下重新构建平台和需要交付的应用，再导出。原有精确 ELF 仍可独立用于符号定位。文档和日志文件不属于此次源码差异导出范围。导出不覆盖同名已有 ZIP。

## 设备记录

启动日志包含 `[firmware] build_id=...`，也可以在 FinSH 输入 `firmware_id` 查询。该值直接读取 Flash 中链接器生成的 GNU note，不从 TF 卡读取。

模块加载记录在重定位、缓存维护及入口解析之后、`module_init` 之前输出；释放记录在 `module_cleanup` 和前置资源清理完成后、代码内存释放之前输出。增加引用或释放一个尚未归零的引用不会产生释放记录。

模块对象中的 `exec_mem_space` 是实际执行区域，`vstart_addr` 是 ELF 的首个装载段地址，`mem_size` 是装载范围。build-id 位于现有 SDK 模块对象中，TF 卡移除后不需要重开文件取身份。完整 dump 中的对象优先于历史串口记录；日志不是独立的模块状态数据库。

## 现场采集

保留完整启动和故障日志，并使用 SDK 已有的 J-Link/AssertDump 路径获取故障寄存器和内存。至少覆盖相关 SRAM 中的模块对象、线程对象和栈；动态模块执行区域在 PSRAM 中，需要同时取得相关 PSRAM 内容。

`saved_stack_frame`、`saved_stack_pointer`、`saved_scb_reg`、`error_reason` 保持 SDK 现有格式。系统断言先保存现场，再执行控制台、系统信息和钩子处理；诊断中再次断言或异常不应覆盖首份终止性断言现场。模块线程断言仍走 SDK 的模块退出分支，退出前打印表达式、函数和行号，不将其当作整机终止。

断言日志给出断言源码位置；软件保存函数的 PC 是保存入口附近的位置，不是硬件精确捕获的原断言指令。调试器读取的当前 PC 则是导出时的停机位置，可能已在后续死循环中。三者必须分别解释。

当前 GCC Cortex-M33 硬浮点路径还保留 `saved_assert_expression`、`saved_assert_function`、`saved_assert_line`，分别指向表达式、函数名和行号。启用 `ASSERT_OPTIMIZE_1` 时，函数为空，最后一个字段保存返回地址。这些变量可从完整 RAM 转储中读取；只导出寄存器不会自动附带全部字符串内容。

卡顿取证继续使用已有的 `HANG_DIAG=1`。监测日志中的线程 SP 是调度保存值，不是运行线程实时 PC。关闭中断或锁住调度的卡死可能无法由监测线程报告，应暂停 CPU 后导出现场。SF32LB57 的 Python 纯 UART 导出能力尚未在本项目确认，本功能不扩展该传输。

## 标准 GDB 操作

1. 根据设备的固件 build-id，从清单选择 `main.elf`，不要自动使用构建目录中的最新文件。
2. 使用 SDK 现有转换方法取得 core，或连接已经暂停的调试目标。
3. 根据同一现场中模块对象的 build-id 选择 `.so.nostrip`。
4. 计算 `load_bias = exec_mem_space - vstart_addr`，为每个涉及的模块和私有库加入符号。

```text
arm-none-eabi-gdb <匹配的main.elf>
(gdb) core-file <SDK转换得到的core文件>
(gdb) add-symbol-file <匹配的books.so.nostrip> -o <books的load_bias>
(gdb) add-symbol-file <匹配的bk_mobi.so.nostrip> -o <私有库的load_bias>
(gdb) info registers
(gdb) bt
(gdb) list *<原始PC地址>
```

`-o` 是整体地址偏移，不是 `.text` 地址。需要按节指定时，用 `arm-none-eabi-readelf -SW` 查看 ELF 节地址，再加上偏移；不要把模块基址直接作为 `.text` 地址。改变开发机源码路径时，使用 `set substitute-path <原路径> <本机路径>`。

只选择与当前栈内存对应的寄存器上下文。SDK 当前单寄存器 core 不会因为执行 `thread apply all bt` 就自动产生全部 RT-Thread 线程。GDB 能展开多少帧，取决于 DWARF、实际栈和优化结果；扫描栈找到的代码地址只能作为候选，不能当作已确认调用链。符号或必要内存缺失时保留原始地址，并记录缺失项。

电脑侧自动附加模块符号、遍历 RTOS 上下文及生成分析报告仍属于后续任务，本次没有实现另一套分析器。

## 显式故障夹具

仅在测试固件中启用：

```powershell
scons -C project --board=dpi-hdk_lb57gyd7n6_epd_hcpu DIAG_FIXTURES=1 HANG_DIAG=1 -j8
```

`diagnostic-fixtures/diagmod.so` 和 `diaglib.so` 是直接供 SDK 加载的测试模块，不是 Launcher 安装包，不加入 `APPS=all`、`PREINSTALL` 或 `fs_root.bin`。将它们复制到测试 TF 卡根目录。以下命令必须主动执行才注入故障，执行断言、非法指令或死循环后可能需要复位。

```text
diag_test load /sdcard/diagmod.so
diag_test dynamic 0
diag_test ref /sdcard/diagmod.so
diag_test close
diag_test private /sdcard/diaglib.so 0
diag_test unload
```

上面这一组验证装载、嵌套私有库调用、未归零引用和清理顺序。重复装卸可以观察不同基址及地址复用。

| 命令 | 故障路径 |
| --- | --- |
| `diag_test static 2` | 固件中静态编译的公共故障函数 |
| 加载后 `diag_test dynamic 2` | 动态模块中同一份源文件的故障函数 |
| 加载后 `diag_test ui 2` | UI 主循环调用动态函数 |
| 加载后 `diag_test worker 2` | 后台线程调用动态函数 |
| 加载后 `diag_test private /sdcard/diaglib.so 2` | 模块调用私有库时的故障 |
| `diag_test phase init 2` 后加载 | 模块初始化中故障，映射日志应已输出 |
| 加载后 `diag_test phase cleanup 2`，再卸载 | 清理中故障，此时不能已有代码释放日志 |
| `diag_test nested 1` | 首次系统断言后，断言钩子再次断言 |
| 未加载时 `diag_test exec /sdcard/diagmod.so` | SDK 模块主线程断言退出 |
| 加载后 `diag_test ui 3` | UI 主循环可调度死循环，配合 hang_diag 取证 |

参数 `0` 正常返回，`1` 断言，`2` 执行未定义指令，`3` 死循环。静态与动态对照都使用 `fault_probe.c`。真实寄存器保存、故障日志、跨模块调用栈、TF 拔卡后的取证仍必须在板上验证；主机测试和编译不能替代这些验证。
