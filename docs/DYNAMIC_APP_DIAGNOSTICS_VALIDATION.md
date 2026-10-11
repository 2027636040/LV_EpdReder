# 动态应用故障诊断验证记录

验证板级：`dpi-hdk_lb57gyd7n6_epd_hcpu`。工具链：Arm GNU 14.2.1。SDK 指针保持 `a25ebccdb986a98070287cf1701dc5adb10a265d`，使用工作区现有补丁与本次诊断修改。没有烧录或在设备上注入故障。

## 主机测试

| 入口 | 已验证内容 | 结果 |
| --- | --- | --- |
| `python -m unittest tools.tests.test_app_build tools.tests.test_debug_artifacts` | 应用选择、正式输出、身份不匹配、私有库、历史符号、缺失归档、源码变化和显式导出、稳定宏顺序 | 19 项通过 |
| `tools/tests/test_module_diagnostics_t2.py --asan` | 真实 ELF note 解析函数；开启/关闭两种真实模块结构；装卸函数及引用、嵌套加载、地址复用、短读 | 317 个边界/身份用例和 20,000 次变异输入通过，生命周期检查通过 |
| `tools/tests/test_assert_diagnostics.py --arm-gcc <arm-none-eabi-gcc>` | 真实保存/断言函数；控制台、SDIO、sysinfo、hook、fatal、dump 中二次断言/异常；模块断言退出 | 28 个场景通过；M33 汇编/反汇编检查通过 |

两个 C 主机测试使用 MSVC x86 环境，例如先运行 `vcvars32.bat`。模块测试用实际 SDK 头文件，替换底层装载、RTOS 和缓存服务；断言测试替换硬件寄存器和终止循环。测试证明代码分支和保存顺序，不证明真实设备寄存器或跨模块栈已经正确导出。

实际 ELF 类型信息确认 `struct stack_frame` 为 72 字节。汇编在基本保存帧之外保留 8 字节的 r4/LR 备份，进入 C 函数时维持 8 字节栈对齐。软浮点路径没有启用本次保存扩展。

## 编译与资源

开启和关闭 `CONFIG_RT_USING_MODULE_DIAG` 分别完成以下构建：

```powershell
scons -C project --board=dpi-hdk_lb57gyd7n6_epd_hcpu APPS=all DIAG_FIXTURES=1 HANG_DIAG=1 -j8
```

两次均包含 weather、books、gallery、words 和 books 的私有库 `bk_mobi`。正式应用的导入符号检查通过。故障模块单独位于 `diagnostic-fixtures/`，不在安装目录中。

| 等配置比较项 | 诊断关闭 | 诊断开启 | 差值 |
| --- | ---: | ---: | ---: |
| GNU size 的 `.text` 输出节 | 1,905,972 B | 1,906,796 B | +824 B |
| 主程序与资源二进制合计 | 7,570,766 B | 7,571,590 B | +824 B |
| `.data` | 4,248 B | 4,248 B | 0 |
| `.bss` | 101,104 B | 101,104 B | 0 |
| `struct rt_dlmodule` | 120 B | 144 B | 每个已加载模块 +24 B |

其余静态 RAM/PSRAM 输出节在这两次构建中相同。24 字节随模块对象分配和释放，不是全局预留表。比较双方都已包含首次断言修复与固件身份，因此表中差值只代表模块诊断开关，不能作为本次全部功能相对旧固件的总增量。

首次断言另新增 3 个 4 字节源码信息字段及 1 个 4 字节保护标志，合计 16 字节静态 RAM。主固件 GNU note 为 36 字节；其 SHA1 身份在 `main.bin/ER_IROM1.bin` 的偏移 532 处已经核实。SDK 概览的 ROM 分类值与最终烧录二进制长度不同，此处采用实际二进制长度比较。

`readelf` 确认安装模块仍有 `.dynsym`、重定位及 GNU note，未裁剪模块保留 DWARF。`diagmod.so` 的 ELF 入口为 `main` 的 Thumb 地址，用于验证 SDK 模块主线程断言退出，不能使用普通 GUI 库的零入口。

## 正式产物与增量构建

诊断开关最终恢复为开启，分别完成平台独立构建和应用批量构建：

```powershell
scons -C project --board=dpi-hdk_lb57gyd7n6_epd_hcpu -j8
scons -C modules APPS=all -j8
```

正式固件不包含故障夹具，也没有启用测试用 `HANG_DIAG=1`。实际符号表确认不存在 `diag_fixture`、`diag_probe` 测试入口。主程序与资源二进制合计 7,569,058 字节。当前固件 build-id 为 `4f02238be7123416dfe36a45ddf8fc187b1bfeaf`。

随后以 `scons -C modules APPS=weather -j8` 检查两种缺失产物情形：

- 将安装用 `weather.so` 移到临时备份目录后，构建从现有 `.so.nostrip` 复制并裁剪，没有重新链接。
- 将 `weather.so.nostrip` 移到临时备份目录后，构建重新链接一次并恢复调试文件；重建内容相同时，SCons 保留内容相同的安装文件。

两次均成功，最终安装文件与调试文件身份一致。宏定义排序固定后，不再因集合遍历顺序变化导致动态模块反复编译。

产物均位于 `project/build_dpi-hdk_lb57gyd7n6_epd_hcpu/`：

| 路径 | 内容 |
| --- | --- |
| `main.elf`、`main.bin/` | 正式固件符号与烧录镜像 |
| `app-resources/` | weather、books、gallery、words 安装包；不含 `.nostrip` |
| `debug-artifacts/manifest.json` | 固件、应用、私有库、配置和源码身份清单 |
| `debug-artifacts/symbols/<build-id>/` | 按身份保留的完整符号及配套配置 |
| `debug-bundle.zip` | 显式导出的本次配套调试包 |

实际执行以下命令成功：

```powershell
python tools/debug_artifacts.py --build project/build_dpi-hdk_lb57gyd7n6_epd_hcpu --output project/build_dpi-hdk_lb57gyd7n6_epd_hcpu/debug-bundle.zip
```

调试包共 51 个文件、15,232,660 字节，包含 6 份 ELF 符号：主固件、四个应用和 `bk_mobi` 私有库。已检查 ZIP 完整性、所有符号与配置摘要、固件 build-id，以及安装目录各模块的 build-id 和摘要。包内保存 Git 版本、未提交差异和未跟踪源码输入；还原完整源码仍需取得清单指定的 Git 版本。

编译记录保存在 `project/build_diagnostics_platform.log`、`project/build_diagnostics_apps.log`、`project/build_diagnostics_on.log`、`project/build_diagnostics_off.log` 和两个 `project/build_diagnostics_missing_*.log` 中。项目与 SDK 的 `git diff --check` 均通过。

## 尚需上板验证

1. 同一故障函数的静态/动态异常，保存寄存器和手工 GDB 栈回溯。
2. UI、后台线程、初始化、清理及私有库故障时的模块映射和源码定位。
3. 首次系统断言后在诊断钩子再次断言，确认导出的第一份现场不被覆盖。
4. SDK 模块主线程断言退出与系统线程断言停止，分别符合既有行为。
5. TF 卡移除后依靠 RAM 中的身份和电脑归档符号取证；真实 SRAM/PSRAM dump 范围。

电脑侧自动附加动态符号和遍历 RTOS 上下文仍未实施。手工操作见 [故障取证与手工定位](DYNAMIC_APP_DEBUGGING.md)。
