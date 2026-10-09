# SD 卡介质组件

`epd-sdcard` 将 SD 卡检测、探卡、FAT 挂载、会话失效和安全卸载封装为固件内的静态组件。实现以 Solution2.0 的 TF 服务为来源，应用退出与加载确认由使用组件的平台完成。

组件源码、最小消费者示例和主机测试已经实现。主工程通过本地 `epd-sdcard/1.0.0` 源码包接入组件，当前板级配置启用该组件；原存储服务不再独立管理 SD 卡。包尚未发布，编译和上板验收结果在主仓库 `docs/SD_CARD_COMPONENT_TODO.md` 中维护。

## 仓库内容

| 文件 | 用途 |
| --- | --- |
| `conanfile.py` | SF-PKG/Conan 源码包 recipe，复用 SDK 的 `SourceOnlyBase`。 |
| `conan.lock` | Conan 生成的组件开发依赖锁文件，固定基础包版本及 recipe revision。 |
| `conandata.yml` | Solution 来源提交、文件范围和所需 SDK 接口。 |
| `Kconfig` | 组件开关及 SDIO、GPIO、DFS/FAT 依赖，默认关闭。 |
| `SConscript` | 显式编译组件 worker 和 SDK 适配两份源文件，不自动编入示例与测试。 |
| `include/sdcard.h` | 启动配置、状态快照和带请求号的释放确认接口。 |
| `src/sdcard.c` | 检测 IRQ、稳定采样、挂载、会话失效、等待释放和卸载。 |
| `port/sdcard_sifli_sdio.c` | 绑定指定 SD host，转调 SDK 请求接口并读取实际结果。 |
| `LICENSE` | Solution TF 派生代码的 SiFli 许可条款。 |
| `example/consumer/sf-pkg.yaml` | 本地包依赖示例，不会被当前主工程自动启用。 |
| `example/sdcard_demo.c` | 不依赖 Launcher 或 LVGL 的单一文件使用者示例。 |
| `tests` | 元数据、真实 SDK 请求核心、组件 worker 和适配测试，以及板级独立编译工具。 |

源码仅在本目录维护。项目通过 SF-PKG 消费源码包，Conan 缓存和构建目录是生成物。无需额外创建 JSON 注册表，也不在另一个仓库维护第二份 SD 实现。

## 构建契约

当前目标板为 `dpi-hdk_lb57gyd7n6_epd_hcpu`，使用原生 SDIO SD 卡接口和 RT-Thread DFS FAT 文件系统。Kconfig 只约束组件实际使用的依赖，不改变总线位宽、SDIO 时钟或检测脚配置。

必要条件为 `RT_USING_PIN`、`RT_USING_SDIO`、`BSP_USING_SD_LINE`、`RT_USING_DFS`、`RT_USING_DFS_ELMFAT`，以及 `SDIO_CARD_MODE=0`、有效的 `SD_INSERT_DETECT_PIN`。平台传入设备名、挂载点、检测电平和共享 I/O 锁。

基础 SDK 为 2.5.0、提交 `a25ebccdb986a98070287cf1701dc5adb10a265d`，还需要以下本地补丁；版本号检查不能代替接口要求：

- 已有 `sifli_sdio_sdcard_get_host()` 和无卡启动返回有效 host 的修复。
- 已实现的 `mmcsd_change_request()` 和 `mmcsd_change_state_get()`，用于按 host 查询请求状态及检查提交失败。

组件不会直接导出给动态应用。平台继续通过现有 `storage_*` 接口服务应用；介质会话与退出握手不依赖 Launcher 的内部对象。

### SDK 探卡接口约定

每个控制器独立保存 `NONE`（尚未请求）、`PENDING`（正在排队或处理）、`DONE`（处理结束）三个状态。状态保存在 `rt_mmcsd_host` 中；调用者通过 `mmcsd_change_state_get(host)` 读取，不直接修改字段。

`mmcsd_change_request(host)` 不等待探卡结束。返回 `RT_EOK` 表示请求已投递；同一 host 已有请求时返回 `-RT_EBUSY`，不会重复入队。邮箱满或其他投递错误会原样返回，并恢复投递前的状态。因为投递可能唤醒检测线程，函数成功返回时状态也可能已经是 `DONE`，不能假设一定仍是 `PENDING`。

检测线程完成探卡或移除、释放 host 锁后才发布 `DONE`。探卡失败、初始化失败、SDK 不支持移除的 SDIO 卡也会完成请求；调用者必须另行判断 SD 卡、块设备和挂载结果。`DONE` 保留到下一次请求，便于组件接管 SDK 已发起或已完成的启动探卡。

介质变更必须由单一所有者串行发起，否则读到的状态可能属于后续请求。SDK 保留原有语义：`host->card` 为空时执行探卡，不为空时执行移除；本接口不是“确保卡已初始化”的幂等操作。移除请求必须在文件系统卸载后发起，host 的释放仍由驱动管理，不应凭 `DONE` 直接释放 host 或其资源。

旧 `mmcsd_change()` 转调新入口，失败时记录警告。旧 `mmcsd_get_stat()`、`mmcsd_set_stat()` 保留兼容性：探卡结束置 1，移除分支结束置 0；它们仍是全局标志，不提供控制器隔离。组件和平台只使用按 host 的接口，不消费或清空全局热插拔邮箱。组件没有自动初始化入口，未启用或未调用 `sdcard_start()` 的 SDK 工程不依赖本组件，也不需要修改原 SD 调用代码。

## 运行接口和所有权

`sdcard_start()` 从线程启动常驻服务；返回成功仅表示线程已经启动，不表示挂载成功。配置结构会被复制，但设备名、挂载点字符串及共享互斥锁必须保持有效。检测脚必须与 SDK 的 `SD_INSERT_DETECT_PIN` 一致；当前 SDIO 启动路径使用低电平表示有卡。板级代码负责启动探卡前的引脚复用和上拉，以及其他设备初始化后必要的复用恢复。组件只设置输入模式并接管检测 IRQ，不改变 SD 时钟、位宽或控制器初始化。

同一个 host 和挂载点只能有一个生命周期管理者。平台接入时必须停用原 SD 检测线程、初始挂载和重复探卡代码，不能在它们旁边再启动组件。现有其他控制器及其他工程的 SD 调用者不需要改用本接口。

`sdcard_get_snapshot()` 复制一致的状态快照，不返回内部对象。平台使用 `revision` 判断是否需要处理变化；`present` 是最近的稳定采样结果，`mounted` 是实际挂载事实，`available` 才是允许开始新访问的条件。检测到拔卡的 IRQ 会立即清除 `available` 并使旧 `session` 失效，不等待防抖或 UI 退出。

文件使用者在共享 I/O 锁内，检查 `available` 及创建任务时保存的 `session` 后才能开始新访问。已经进入驱动的 I/O 仍可能因拔卡失败；组件不会取消正在执行的驱动函数。旧使用者可以关闭文件和释放资源，但不能把旧任务重新绑定到新卡。

进入 `DRAINING` 后，平台停止全部相关任务并释放文件、设备访问和异步引用，再从线程调用 `sdcard_release_complete(release_id)`。确认时不得持有共享 I/O 锁。请求号不匹配或已经离开 `DRAINING` 时返回 `-RT_EINVAL`；仍处于 `DRAINING` 的重复确认返回 `-RT_EBUSY`。组件没有强杀应用或强制释放模块的能力。

### 状态和失败处理

| 状态或事件 | 组件处理 |
| --- | --- |
| 开机请求为 `NONE` | 有卡时提交一次探卡；无卡时等待插入 IRQ。 |
| 开机请求为 `PENDING` | 等待已有请求，不再次投递。 |
| 开机请求为 `DONE` | 核查 SD 卡类型和块设备，不将完成直接当作成功，也不立即重试已经失败的探卡。 |
| `READY` | FAT 挂载成功且边沿未变化后发布新会话；重复唤醒不会重复挂载。 |
| `DRAINING` | 等待平台释放确认。35 秒只更新诊断状态，之后继续等待，不卸载仍被使用的文件系统。 |
| `UNMOUNTING` | 在 I/O 锁内核查打开的文件，卸载成功后清除 `mounted`；释放 I/O 锁后才请求 SDK 移除卡设备。 |
| 探卡提交或结果失败、挂载失败 | 进入 `FAULT` 并记录阶段和原始错误；观察到移除后清理旧对象，下一次插入才重试。 |
| 探卡或移除超时 | 保持隔离，不假定 SDK 请求已取消，不再提交变更；恢复需要重启。 |
| 释放等待异常、遗留文件、卸载或移除失败 | 保留实际挂载事实和对象，禁止新卡访问；恢复需要重启。 |
| 退出时插入替换卡 | IRQ 仍然记录边沿；旧卡完成释放、卸载及 SDK 移除后才处理替换卡。 |

稳定采样采用 Solution 的 15 ms 后采样、间隔 3 ms 再确认；采样中出现边沿则重新采样。探卡与移除分别采用 Solution 的 `1000 × 30 ms`、`100 × 30 ms` 等待预算。线程使用当前工程的 3072 字节栈、优先级 22；栈余量和硬件调度仍需上板检查。没有任务时 worker 等待信号量，不固定轮询。

当前验收配置使用 RT-Thread 共享文件描述符表，未启用 LWP。卸载前的句柄检查只核查该表；不能据此宣称支持独立进程文件表。当前适配范围为 SDIO SD 存储卡，不包含 SPI、SDHCI、eMMC、多插槽或睡眠恢复接入。

## 最小消费者示例

`example/sdcard_demo.c` 提供 `sdcard_demo_start()` 和 `sdcard_demo_step()`。在一个没有其他 SD 管理者的测试工程中启用组件，并将该示例源文件加入测试工程；SDK 设备初始化、根文件系统和 `/sdcard` 目录须已准备好，检测脚复用由板级配置完成。

从同一个应用线程执行：

```c
if (sdcard_demo_start() == RT_EOK)
{
    for (;;)
    {
        sdcard_demo_step();
        rt_thread_mdelay(100);
    }
}
```

示例只在状态改变时打印日志；每个新会话读取一次 `/sdcard/readme.txt` 的开头，读取后立即关闭文件。它是唯一文件使用者，因此在后续 step 中收到释放请求时可以确认。实际平台必须先协调全部使用者退出，不能照搬示例直接确认。示例的 100 ms 循环仅用于展示快照消费，不属于组件内部的检测方式。当前固件已由平台启动组件，不要再加入该示例启动第二个管理者。

## 本地包和项目依赖

### 基础包准备

已从官方 `artifactory` 下载并验证基础包，完整引用为：

```text
sf-pkg-base/1.0.0@sifli#804200ec00f8414709d8d76734083c7a
```

下载的 recipe 与 [sf-pkg-base 源码](https://github.com/OpenSiFli/sf-pkg-base/blob/6bb8bb69566ba983fe04a40336ae9aa8f846fe2d/conanfile.py) 在统一换行后内容一致。[外设仓库 BF30A2 recipe](https://github.com/OpenSiFli/SiFli-SDK-Peripherals/blob/41fc17205130b76ca8205e164a6ea08dc6d6368e/camera/bf30a2/conanfile.py) 同样使用 `python_requires` 和 `SourceOnlyBase`。本组件继续复用该基础类的源码打包、目录声明及 SDK 版本检查，不复制这些实现。

在已执行 SDK `export.ps1` 的环境中，从本目录执行以下命令，可准备相同依赖并离线检查 recipe：

```powershell
conan download "sf-pkg-base/1.0.0@sifli#804200ec00f8414709d8d76734083c7a" --only-recipe -r=artifactory
conan inspect . --no-remote --lockfile=conan.lock --format=json
conan graph info . --no-remote --lockfile=conan.lock --format=json
```

`artifactory` 地址为 `https://jfrog.sifli.com/artifactory/api/conan/conan-local`。基础类需要 Python 的 `semantic_version` 模块，当前 SDK Python 环境已具备该依赖。SDK 环境脚本将 `SIFLI_SDK_VERSION` 设为 `2.5`，基础类将其解析为 `2.5.0`。直接执行 Conan 时同样需要正确激活 SDK 环境。

`inspect` 验证 recipe 能加载；`graph info` 还执行基础类的 `validate()`。检查后者时须确认根节点 `info_invalid` 为空，不能只看命令退出码。已验证 `2.5`、`2.5.0` 均通过；`2.4.0` 或缺少版本变量时，根节点明确标记为无效。

`conan.lock` 由 `conan lock create . --no-remote --lockfile-out=conan.lock` 生成，固定组件开发时的基础包解析结果。主工程消费完整组件时仍需单独验证消费者依赖图及锁定方式，见实施清单 SD-06。

### 创建及接入源码包

运行源码、SDK 接口和测试完整后，在已激活的 SDK 环境中，从本目录创建本地包：

```powershell
conan create . --version=1.0.0 --no-remote --lockfile=conan.lock
```

此命令使用上面准备的基础包和锁文件。recipe 的基础类版本约束沿用本地 SDK 模板，具体解析结果由锁文件固定。

本地包创建并通过验证后，将消费示例中的 `requires` 合并到项目目录的 `sf-pkg.yaml`，再运行 SDK 的依赖安装入口。不要直接覆盖项目已有依赖文件：

```powershell
sdk.py sf-pkg install
```

本组件由项目根清单直接声明。首次安装不要指定 `--board`：当前 SDK 会先解析板级配置，包尚未安装时会因无法识别组件开关而失败。先执行上述安装，再执行板级构建即可。该安装入口可能访问配置的 remote；`--no-remote` 是上面 Conan 本地创建命令的参数，并不是此 SDK 安装命令的参数。离线消费和确定版本的验证列在实施清单中。

启用 `CONFIG_PKG_USING_EPD_SDCARD=y` 后，由 SDK 生成的 SCons/Kconfig 依赖文件接入组件。不得再手工把同一源目录加入项目 SConscript。

主工程的 `project/sf-pkg.yaml` 固定本地包的 recipe revision。修改组件后，需重新执行 `conan create`，将输出的新 revision 更新到该清单，再构建项目；只修改源码目录不会自动替换已安装的包。应在创建包和构建固件时使用同一个 SDK 激活环境，避免读到不同的 Conan 缓存。

板级 `board_sdcard.c` 负责检测脚早期复用和 LCD 初始化后的恢复；`storage.c` 保留内部 NOR 挂载及共享 I/O 锁，SD 可用状态、会话和变化版本来自组件快照。平台在异步就绪后刷新目录并调度安装恢复，探卡不会阻止主界面和内部存储使用。应用释放完成后，UI 线程携带已处理的请求号确认，不读取一个新请求号代替旧请求号。

## 注册表发布流程

发布使用 SDK 现有的“上传 Conan 包，再同步组件目录”流程，注册信息来自 recipe，不另建注册服务。发布前需要确定实际账号 namespace、组件源码仓库 URL，补全 recipe 的 `user`、`url` 和 `homepage` 元数据，并完成实施清单中的功能和运行验收。当前不写入假地址或默认占用 `sifli` namespace。

以下是发布操作模板，只有完成包验收并确定实际账号后才执行：

```powershell
sdk.py sf-pkg --user <publisher> build --version 1.0.0
sdk.py sf-pkg --user <publisher> upload --name epd-sdcard/1.0.0@<publisher> --keep
```

`<publisher>` 须替换为实际账号，且与 recipe 的 `user` 一致。SDK 的 build 命令不会自动把所选登录账号写入 recipe。上传后，消费引用同步替换为正式带 namespace 的完整包引用。`--keep` 保留本地包缓存。

## 元数据检查

在 SDK 环境已设置 `SIFLI_SDK` 后，从组件目录执行：

```powershell
python -B -m unittest discover -s tests -v
```

这些测试检查材料和构建契约，不编译固件，也不证明热插拔运行正确。包导出、固件编译和上板结果分别记录在实施清单。

## SDK 请求状态测试

`tests/test_mmcsd_change.c` 直接编译当前 SDK 的 `mmcsd_core.c` 和原始 MMC/SD 头文件，仅替换 RTOS 调度与卡硬件操作。测试覆盖启动请求状态接管、重复提交、邮箱满及错误回退、不同 host 隔离、32 种探测配置，以及检测线程在发送函数返回前完成或接收下一次请求的时序。

在 Windows 的 **x86 MSVC 开发者命令提示符**中设置 `SIFLI_SDK`，从组件目录执行：

```bat
set SIFLI_SDK=D:\program\LV_EpdReder\SiFli-SDK
python -B tests\run_mmcsd_change_tests.py
```

需要 x86 编译环境，因为该 SDK 的邮箱使用 32 位值承载 host 指针。测试在系统临时目录中编译并运行，不向组件目录写入二进制产物。测试中的调度交错是确定性模拟，不验证真实中断、电气时序、文件系统挂载或平台应用退出；这些内容分别在组件接入和上板验收中验证。

## 组件测试和板级独立编译

在 MSVC 开发者环境执行 `python -B tests/run_sdcard_tests.py`，会直接编译运行 `src/sdcard.c` 和 `port/sdcard_sifli_sdio.c` 的测试入口，替换 RTOS、引脚、DFS 和 SDK 调用结果。worker 的 6 组测试覆盖初始化失败回收、开机接管、会话及确认、防抖和处理期间换卡、可恢复失败，以及不可恢复失败隔离；适配测试检查 host 选择、请求进行中不读取卡对象、卡与块设备结果区分和错误转交。

在当前 SDK ARM GCC 环境中，可以使用已生成的板级编译数据库独立编译组件：

```powershell
python -B tests/compile_board_sources.py ../../project/build_dpi-hdk_lb57gyd7n6_epd_hcpu/compile_commands.json
```

脚本复用实际 `storage.c` 的编译参数、头文件和最终板级配置，额外开启 `-Wall -Wextra -Werror`。worker、适配和示例均已通过；MSVC 组件测试使用 `/W4 /WX` 并通过。临时目标文件自动清理，不链接或覆盖固件，不启用第二个 SD 管理者。SDK 核心请求测试仍有 SDK 原有的类型转换、函数内 extern 和未使用参数警告。

独立编译只证明当前头文件、接口和目标代码能够编译，主机测试只验证模拟的执行交错。完整平台接入后的链接、真实文件系统及热插拔验证仍按实施清单执行。

## 来源与许可

TF 服务来源为 Solution2.0 的 `solution/components/tf`，提交及文件清单见 `conandata.yml`。SiFli 许可原文见 `LICENSE`；移植源文件继续保留各自原始版权声明。

