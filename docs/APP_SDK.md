# 独立动态应用开发包

`tools/app_sdk.py` 从已经构建的平台产物导出与该固件匹配的应用开发包。导出后可以搬移开发包，在工程外编译、检查并打包 C 语言动态应用；构建只需要 Python 3.10 以上和 profile 记录的 Arm GNU 工具链版本。

## 导出

在 PowerShell 中激活工具链，并指定已有的板级构建目录：

```powershell
cd D:/program/LV_EpdReder
. ./SiFli-SDK/export.ps1
python -B tools/app_sdk.py export --firmware project/build_dpi-hdk_lb57gyd7n6_epd_hcpu --output C:/app-dev/dpi-sdk
python -B tools/app_sdk.py export --firmware project/build_sf32lb57-lcd-gyd7n6_epd_hcpu --output C:/app-dev/lcd-sdk
```

每个输出目录必须尚不存在。导出读取 `main.elf`、`rtua.py`、`rtconfig.h`、`cconfig.h`、`app-profile.json` 和 `epd_app_profile.h`。工具检查 profile 自身的 SHA-256 指纹、配置头摘要、公共头摘要以及固件内的 profile 指纹；公共头有变化时，需要先重新构建平台。

导出使用 GCC 预处理器获取公共头在当前配置下的全部递归依赖，包括通过宏选择的头文件。`epd_app.h` 新增的包含文件也会被自动收集。需要使用其他公共接口时，可重复指定 `--header`，例如 `--header cJSON.h`；其参数可以是仓库内路径或板级包含目录中的头文件名。

开发包包含以下内容：

| 路径 | 内容 |
| --- | --- |
| `config/` | 原样保存的板级配置、profile 及应用指纹头。 |
| `include/` | 公共头和编译器解析出的依赖头，保留相对目录结构。 |
| `sdk.json`、`rtua.json` | 固定的编译链接参数、宏、相对包含路径及文件摘要。 |
| `firmware-exports.txt` | 从固件导出表地址范围内的 `__rtmsym_*` 项提取的可导入符号名单。 |
| `app_sdk.py`、`package_app.py` | 独立构建、检查和应用目录打包工具。 |
| `examples/hello/` | 基础页面和第二页跳转、返回示例。 |
| `examples/netdemo/` | 受平台管理的网络等待任务与网络状态快照示例。 |
| `examples/cfgdemo/` | 应用数据目录内的本地配置读写示例。 |

原工程路径不写入构建配置。开发包不携带固件 ELF、固件源代码、完整 SiFli SDK 或工具链；`sdk.json` 保留导出来源 ELF 和 rtua 的摘要。

## 工程外开发

将开发包复制到任意目录，例如 `C:/app-dev/moved-sdk`。安装与 `sdk.json` 的 `compiler` 字段相同版本的 Arm GNU 工具链，将工具链 `bin` 加入 PATH，或对需要工具链的命令指定 `--toolchain`。后续命令不需要设置 `SIFLI_SDK`、`BSP_ROOT` 或 `RTT_ROOT`。

```powershell
cd C:/app-dev
python -B moved-sdk/app_sdk.py check-sdk
python -B moved-sdk/app_sdk.py init --example hello --output hello
python -B moved-sdk/app_sdk.py build --app hello --output build-hello
python -B moved-sdk/app_sdk.py check --module build-hello/hello.so
python -B moved-sdk/app_sdk.py package --app hello --module build-hello/hello.so --output package-hello
```

工具默认使用脚本所在的开发包；也可以传入 `--sdk`。`init --example` 可以选择 `hello`、`netdemo` 或 `cfgdemo`，默认选择 `hello`。`build` 编译应用 `src/` 下的全部 `.c` 文件，并增加应用的 `src/` 和 `include/` 搜索路径。构建、初始化和打包输出均使用新目录。修改应用后可以选择新的输出目录。

构建输出包括可安装的 `hello.so`、保留调试信息的 `hello.so.nostrip` 以及包含实际导入符号和模块摘要的 `build-report.json`。构建会检查编译器报告的非工具链依赖全部位于开发包或应用目录中。应用必须使用 `EPD_APP_DEFINE("hello")`，并实现 `app_main(intent_t)`；更改应用 ID 时，应同时修改 `app.json`、描述符及页面注册中的 ID。

hello 的页面回调在 `GUI_APP_MSG_ONSTART` 创建文字、标题和返回按钮，在 `GUI_APP_MSG_ONRESUME` 更新标题。点击按钮会打开第二页，返回按钮回到上一页。页面与输入组的释放由平台页面生命周期负责。

其余示例使用同一构建和打包流程：

```powershell
python -B moved-sdk/app_sdk.py init --example netdemo --output netdemo
python -B moved-sdk/app_sdk.py build --app netdemo --output build-netdemo
python -B moved-sdk/app_sdk.py package --app netdemo --module build-netdemo/netdemo.so --output package-netdemo
python -B moved-sdk/app_sdk.py init --example cfgdemo --output cfgdemo
python -B moved-sdk/app_sdk.py build --app cfgdemo --output build-cfgdemo
python -B moved-sdk/app_sdk.py package --app cfgdemo --module build-cfgdemo/cfgdemo.so --output package-cfgdemo
```

`netdemo` 通过 `epd_app_background` 声明受平台管理的任务和网络订阅。任务读取 `network_snapshot()`、发布状态文字后，使用 `epd_service_wait()` 等待网络变化或停止事件；平台停止应用时，任务检查取消状态并返回。页面每秒复制一次网络快照并显示可用状态、网络来源和更新代数，暂停页面时停止定时刷新，页面销毁时释放定时器。示例无需固定公网服务，也不发送 HTTP 请求。

`cfgdemo` 通过 `storage_app_path(..., STORAGE_APP_DATA, "settings.bin")` 获取当前应用的数据文件路径。点击按钮会递增计数，并使用 `storage_record_save()` 保存带版本号与校验值的配置记录；保存成功后才更新页面内的计数。重新打开应用时，`storage_record_load()` 读取配置；文件缺失或记录无效时使用零计数。保存失败会显示错误。存储切换期间不提交保存。

## 检查与安装包

构建采用 profile 的 `module_cflags` 和 `module_ldflags`，补充 SDK `BuildLibrary` 使用的 `-e 0` 和 `--build-id=sha1`，并按 SDK 参数剥离安装 ELF。生成共享模块时允许保留未定义符号，之后逐项检查这些符号是否存在于固件的 RT-Thread 导出表。

`check` 验证模块为具有有效可加载段的 ARM 32 位小端共享 ELF，检查应用指纹、入口和描述符符号，以及全部导入。`package` 先执行相同检查，再复用平台的应用打包规则生成 `app.json`、`files.sha256` 和 `<id>.so`；应用目录下存在 `res/` 或 `icon.ezip` 时会一并打包。将完整应用目录交给平台现有应用安装流程。

指纹用于匹配具体固件配置。两块板分别导出的开发包应与对应固件配套使用；更新平台接口或配置后，需要重新导出开发包并重建应用。文件摘要用于发现开发包文件损坏，不提供发布者签名认证。主机上的编译和 ELF 检查不执行应用页面，设备上的加载与显示仍需要运行验证。

## 工具测试

```powershell
python -B -m unittest discover -s tools/tests -p test_app_sdk.py -v
$env:EPD_APP_SDK_BUNDLES = 'C:/app-dev/dpi-sdk;C:/app-dev/lcd-sdk'
python -B -m unittest discover -s tools/tests -p test_app_sdk.py -v
```

未设置 `EPD_APP_SDK_BUNDLES` 时，测试执行配置、路径、导入和指纹等单元检查，并明确跳过真实编译测试。设置该变量后，测试将开发包复制到带空格的临时目录，禁止子进程中的 Python 读取原仓库，清除原工程环境变量，然后实际构建、检查和打包三个示例；测试还验证未导出符号和跨板指纹会被拒绝。
