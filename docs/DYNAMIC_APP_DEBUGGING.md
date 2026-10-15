# 动态应用故障取证与手工定位

## 发布文件

固件和动态应用沿用 SDK 构建入口。固件生成 `main.elf`，模块生成安装用 `.so` 和带完整调试信息的 `.so.nostrip`。

开发者保存每次发布的主固件、应用包、模块和私有库符号、源码及构建配置。故障分析使用设备实际运行版本的配套文件。仓库内模块的符号位于 `modules/<应用ID>/output/`，私有库位于对应库的 `output/`；这些目录会在后续构建时更新，发布时由开发者留存。

应用清单中的平台兼容指纹用于判断应用与固件是否配套，不用于确认具体的应用源码版本。安装包目录仍为 `project/build_<board>/app-resources/<应用ID>/`。

## 现场采集

保留完整启动和故障日志，并使用 SDK 已有的 J-Link、AssertDump 或对应芯片支持的导出方式获取寄存器和内存。现场应包含相关线程对象、线程栈、模块对象，以及动态模块所在的 SRAM 或 PSRAM。

系统异常和断言沿用 SDK 的处理流程。动态模块主线程触发断言时，SDK 按模块线程退出分支处理；主 UI 线程或其他系统线程中的终止性故障按系统故障路径处理。

SDK 的 `saved_stack_frame`、`saved_stack_pointer`、`saved_scb_reg` 和 `error_reason` 用于解释已保存现场。调试器读到的当前 PC 可能处于后续等待循环；断言日志中的源码位置、SDK 保存的寄存器和暂停时的寄存器应分别核对。

可选的 `HANG_DIAG=1` 继续使用已有 UI、绘图和显示阶段监测。监测日志中的线程 SP 是调度保存值，并不是运行线程的实时 PC；处理器无法调度时，使用调试器暂停后导出现场。

## 模块地址与符号

SDK 的 `rt_dlmodule` 对象保存模块执行地址 `exec_mem_space` 和 ELF 首个装载段地址 `vstart_addr`。当前连续 RAM 装载路径的地址偏移为：

```text
load_bias = exec_mem_space - vstart_addr
```

通过调试器或包含模块对象的转储取得地址，为涉及的应用和私有库分别加载其发布时的符号：

```text
arm-none-eabi-gdb <配套main.elf>
(gdb) core-file <SDK转换得到的core文件>
(gdb) add-symbol-file <配套books.so.nostrip> -o <books的load_bias>
(gdb) add-symbol-file <配套bk_mobi.so.nostrip> -o <私有库的load_bias>
(gdb) info registers
(gdb) bt
(gdb) list *<原始PC地址>
```

`-o` 表示整体地址偏移，不是 `.text` 的运行地址。需要按节指定时，先用 `arm-none-eabi-readelf -SW` 查看节地址，再加上装载偏移。开发机源码路径变化时，使用 `set substitute-path <原路径> <本机路径>`。

调用栈分析应使用与栈内存对应的寄存器上下文。SDK 的单寄存器 core 不会自动包含所有 RT-Thread 线程上下文；栈扫描中的候选地址也不等于已经还原的调用链。

## 设备恢复

启动受到问题应用影响时，使用[应用恢复入口](RECOVERY.md)进入内置管理界面，处理应用包后重新启动。恢复模式与故障现场采集分别使用各自已有的接口。
