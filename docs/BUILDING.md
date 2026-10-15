# 平台与动态应用构建

## 首次准备

在仓库根目录使用仓库锁定的 SDK，而不是远端最新 SDK：

```powershell
git submodule update --init --recursive
. .\SiFli-SDK\export.ps1
python tools/prepare_build.py
```

准备脚本先检查 SDK 基线，应用 `patches/SiFli-SDK/` 中的版本化补丁，再创建本地
SD 卡组件包，并运行 `sdk.py sf-pkg install`。此时不传 `--board`，确保组件的
Kconfig 在第一次板级配置解析前就已生成。SDK 补丁列表及上游评审见
`patches/SiFli-SDK/series.json`。重复执行会跳过已应用的补丁；遇到冲突会停止，
不覆盖现有 SDK 修改。首次运行需要 Conan 可访问 SDK 配置的 `artifactory` 基础包源。

SDK 基线、补丁或 SD 组件更新后重新执行准备脚本，普通增量构建无需执行。
SDK 子模块指针与项目配置是一组：不要使用 `git submodule update --remote`，也不要
单独切换 SDK 分支。新 SDK 的 Tiny TTF 配置定义与当前项目不同，混用会触发重复
choice 警告。

下列命令均从仓库根目录执行。已经进入 `project/` 时，去掉 `-C project`；不要让
SCons 尝试进入不存在的 `project/project`。

## 平台单独编译

```powershell
scons -C project --board=dpi-hdk_lb57gyd7n6_epd_hcpu -j8
```

默认构建平台固件及配套启动镜像、公共资源、波形和 `fs_root.bin`，不编译动态应用。
文件系统镜像默认只包含 `disk/` 初始内容，使用 SDK 自带的 `tools/mklfsimg/mklfsimg.exe` 生成，
不在本机编译制镜工具，不需要 Visual Studio 或 C++ Build Tools。
平台不依赖 `modules/` 中的应用源码、单词词库、Node.js 或 `sharp`。
固件产物和下载脚本位于 `project/build_dpi-hdk_lb57gyd7n6_epd_hcpu/`。
普通下载不包含 `fs_root.bin`；生成镜像与是否烧录镜像分别控制。

LCD DevKit 载板连接同款墨水屏时，使用项目侧板级配置：

```powershell
scons -C project --board=sf32lb57-lcd-gyd7n6_epd_hcpu --board_search_path=../boards -j8
```

`APPS`、`PREINSTALL`、`STORAGE_IMAGE` 的用法不变。产物目录改为
`project/build_sf32lb57-lcd-gyd7n6_epd_hcpu/`；单独构建应用时需指定
`FIRMWARE=build_sf32lb57-lcd-gyd7n6_epd_hcpu`。引脚和硬件配置见
[LCD DevKit 板级说明](../boards/sf32lb57-lcd-gyd7n6_epd/README.md)。

EPD DevKit 专用载板使用同样的项目侧板型搜索方式：

```powershell
scons -C project --board=sf32lb57-epd-gyd7n6_hcpu --board_search_path=../boards APPS=all PREINSTALL=weather,books STORAGE_IMAGE=1 -j8
```

该命令编译平台与六个正式应用，只预装天气和书架。产物位于
`project/build_sf32lb57-epd-gyd7n6_hcpu/`；单独编译应用时指定
`FIRMWARE=build_sf32lb57-epd-gyd7n6_hcpu`。只需要平台时省略 `APPS`、
`PREINSTALL` 和 `STORAGE_IMAGE`。引脚和外设配置见
[EPD DevKit 板级说明](../boards/sf32lb57-epd-gyd7n6/README.md)。

## 单个、批量和全部应用

应用构建读取已完成的平台构建，不重新编译主固件：

```powershell
scons -C modules APPS=weather -j8
scons -C modules APPS=books,gallery -j8
scons -C modules APPS=all -j8
```

`APPS` 支持 `weather`、`books`、`gallery`、`words`、`weread`、`board`，多个 ID 用逗号分隔。
`all` 明确选择上述六个应用；不传参数或使用 `none` 不构建应用。
未选择的应用不检查源码和私有依赖；已选择的应用缺少源码或必需资源会报错，不静默跳过。
新增正式应用时，在 `tools/native_apps.py` 注册该应用的资源和打包规则，即可复用两个入口。
`modules/hello` 是独立开发示例，构建方式见 [应用开发](APPLICATION_DEVELOPMENT.md)。

默认读取 `project/build_dpi-hdk_lb57gyd7n6_epd_hcpu`。选择其他已构建固件时：

```powershell
scons -C modules APPS=weather FIRMWARE=build_<board> -j8
```

`FIRMWARE` 可填 `project` 下的目录名，也可填绝对路径。
该目录必须包含 `main.elf`、`rtconfig.h`、`cconfig.h`、`rtua.py`、`app-profile.json` 和 `epd_app_profile.h`。
旧构建目录缺少 `rtua.py` 时，先重新运行一次平台构建。
应用沿用所选固件的头文件路径、编译配置和兼容标识，打包前核对主固件的导出符号。
SDK、平台公开接口或编译配置改变后，应先更新平台，再重新构建需要部署的应用。

单词应用额外需要 Node.js 和 `sharp`。全量预生成词库及来源报告已保存在
`modules/words/assets/dictionary/` 并纳入 Git，克隆后无需另外取得词库。构建使用 Python 标准库
自动解压并校验完整 WDB，不下载原始资源、不重新建立索引，详见
[单词应用](../modules/words/README.md)。仅构建其他应用时不需要这些依赖。
选择其他随包词库时，在构建命令中增加 `WORDS_DICTIONARY=<词库绝对路径.wdb>`。

项目构建必需的源码、资源和预生成输入由 Git 版本控制，不以开发者本地的 `output/` 或
`build_*/` 目录作为唯一来源。SDK 沿用子模块管理，工具链及宿主工具按构建环境安装；
编译产生的镜像、动态模块、安装包和可重建的中间文件仍放在忽略的输出目录。

两个构建入口共用打包逻辑，完整安装包统一输出到所选固件目录：

```text
project/build_<board>/app-resources/<应用ID>/
```

源码和资源变化后，再次选择对应应用构建，会更新该应用的完整目录。
未选择应用的旧输出不会删除，也不会更新；不要把旧输出当作本次重新验证过的安装包。
将完整应用目录复制到 TF 卡的 `apps/<应用ID>/`，再通过设备“应用安装”安装。
单独进入 `modules/<应用ID>` 编译只产生模块中间产物；需要可安装目录时使用上面的统一入口。

也可以在一次命令中构建平台与指定应用：

```powershell
scons -C project --board=dpi-hdk_lb57gyd7n6_epd_hcpu APPS=weather,books -j8
```

## 出厂文件系统与预装列表

应用构建列表和出厂预装列表分别指定。单独构建应用包不生成文件系统镜像。
平台构建始终生成镜像，但只有显式初始化或迁移选项才将镜像加入下载清单。

```powershell
# 生成仅包含 disk/ 的镜像，并加入下载清单
scons -C project --board=dpi-hdk_lb57gyd7n6_epd_hcpu STORAGE_IMAGE=1 -j8

# 生成含所选应用的预装镜像，不要求另外传 APPS，也不加入下载清单
scons -C project --board=dpi-hdk_lb57gyd7n6_epd_hcpu PREINSTALL=weather,books -j8

# 生成相同预装镜像，同时加入下载清单供初始化设备
scons -C project --board=dpi-hdk_lb57gyd7n6_epd_hcpu STORAGE_IMAGE=1 PREINSTALL=weather,books -j8
```

`PREINSTALL` 使用与 `APPS` 相同的 ID 规则，可以独立使用。
同时传入 `APPS` 和 `PREINSTALL` 时，构建两者的并集，但只把 `PREINSTALL` 加入镜像。
`disk/` 中手动放入的文件仍会包含在镜像中。
预装内容必须能够放入实际文件系统分区；`PREINSTALL=all` 不保证容量足够。

旧设备迁移仍使用 `STORAGE_IMPORT=<备份绝对目录>`，可以显式附加预装列表。
迁移保持备份内已有应用目录，不用预装包覆盖它们；旧包通过设备安装流程更新。
具体迁移步骤见 [存储迁移](STORAGE_MIGRATION.md)。

`STORAGE_IMAGE=1` 或 `STORAGE_IMPORT` 会把 `fs_root.bin` 加入下载清单，烧录后覆盖内部文件系统，不能用于保留现有用户数据的普通升级。
完成初始化或迁移后，重新运行不带存储选项的平台构建，恢复普通下载清单。
需要烧录时，在仓库根目录运行 `project\build_dpi-hdk_lb57gyd7n6_epd_hcpu\uart_download.bat`。

SDK 原生制镜工具使用 Windows 系统 ANSI 编码保存文件名。非 UTF-8 系统环境下，镜像中的非 ASCII
文件名可能与设备要求的 UTF-8 不一致，构建会明确报错。预置文件可使用英文文件名；中文文件内容不受影响。
这一边界仅影响电脑端制镜，不改变设备运行时读取 TF 卡中文文件名的能力。

## 动态应用调试符号

SDK 构建生成固件 `main.elf`、模块 `.so` 和带完整调试信息的 `.so.nostrip`。
开发者应保存每次发布的配套文件、源码和配置。现场采集与符号定位复用 SDK 工具和 GDB，见
[动态应用故障取证](DYNAMIC_APP_DEBUGGING.md)。

## 独立开发与发布

不包含平台仓库的独立应用工程使用 [应用开发包](APP_SDK.md)。配置式信息看板可先用
[桌面预览](BOARD_TEMPLATES.md) 检查布局，再构建同一份布局代码。
完整构建结束后，使用 [发布目录工具](RELEASING.md) 核对所有待交付应用的兼容标识和资源摘要。
