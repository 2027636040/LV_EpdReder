# 平台与动态应用构建

在仓库根目录激活当前 SDK 环境：

```powershell
. .\SiFli-SDK\export.ps1
```

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

## 单个、批量和全部应用

应用构建读取已完成的平台构建，不重新编译主固件：

```powershell
scons -C modules APPS=weather -j8
scons -C modules APPS=books,gallery -j8
scons -C modules APPS=all -j8
```

`APPS` 支持 `weather`、`books`、`gallery`、`words`，多个 ID 用逗号分隔。
`all` 明确选择上述四个正式应用；不传参数或使用 `none` 不构建应用。
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
