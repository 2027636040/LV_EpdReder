# 平台建设验收记录

## 范围与环境

本记录对应 2026-10-11 的本地平台完善，覆盖独立应用开发包、恢复模式、内容交接、共享图片依赖、摘要看板、桌面预览、发布目录与文档。任务分解见[实施清单](PLATFORM_IMPLEMENTATION_TODO.md)。

主机环境为 Windows、Python 3.13、MSVC 19.44。目标编译使用当前仓库固定的 SiFli SDK 及其既有本地修复，通过 `export.ps1` 激活工具链。本次平台功能在项目侧实现；SD 组件保留本地可构建、待发布状态。

## 主机验证

| 验证对象 | 入口 | 结果 |
| --- | --- | --- |
| 恢复模式 | `python tests/recovery/run_tests.py` | 普通/强制恢复及 finsh 开关的四种组合通过；覆盖读写失败、下次启动语义、装载前拦截与内置管理入口。 |
| 应用服务和存储 | `python tools/tests/run_app_storage_tests.py` | 服务、卡状态、退出交接、模块引用、RAM 统计五组通过；包括摘要副本和相同内容不发布变更。 |
| UI 工作期限 | `tools/tests/reader_public` 的 `ui_deadline_100`、`ui_deadline_1000` | 两种 tick 频率通过，未改变现有分步任务期限语义。 |
| 内容交接 | `python tools/tests/run_app_content_tests.py --asan` | 四组通过，复用真实 SDK intent 与重启判定，覆盖文件类型发现、重复打开、旧卡会话、页面销毁和书架数据；启用地址检测。 |
| 类型声明 | `test_content_manifest.py` | 两项通过，检查源清单验证与安装清单保留。 |
| 看板和预览 | `test_board_preview.py` | 十项通过，复用设备布局 C 代码及 SDK cJSON，覆盖分配失败、越界、摘要和 SVG 输出。 |
| 发布目录 | `test_release_catalog.py` | 共 23 项；Windows 通过 21 项，跳过两项符号链接权限测试；Linux 通过 21 项，跳过两项 Windows junction 测试，互补覆盖全部用例。包含索引格式、声明负载、摘要一致性和发布目录边界。 |
| 构建和打包 | `test_app_build.py`、`test_package_capacity.py`、`test_debug_artifacts.py` | 分别十项、一项、十一项通过；包括未提供独立图标的应用打包。 |
| 独立开发包 | `test_app_sdk.py` | 十七项通过，无跳过；两板各三个示例在移位目录真实编译、检查与打包，禁用 Python 对原仓库的读取。 |
| 文档 | `test_documentation.py` | 所有注册应用都有设计文档；维护入口的本地链接可达。 |

Python 单元测试通过 `python -B -m unittest discover -s tools/tests -p <文件名> -v` 运行。C 测试需要 MSVC 开发者环境；Linux 图片测试使用原有 GCC/Clang 与地址、未定义行为检测入口。

公共图片迁移对 58 个受版本管理文件核对内容一致，图片、封面、依赖许可和打包回归通过。SD 组件发布元数据、工作线程、适配及 MMCSD 状态测试通过；正式发布检查仍会指出缺少真实发布命名空间和仓库资料，没有修改当前依赖锁定。

## 目标编译与开发包

以下命令均在激活 SDK 环境后、仓库根目录执行并通过：

```powershell
scons -C project --board=dpi-hdk_lb57gyd7n6_epd_hcpu APP_RECOVERY=1 -j8
scons -C project --board=dpi-hdk_lb57gyd7n6_epd_hcpu APPS=all PREINSTALL=weather,books STORAGE_IMAGE=1 -j8
scons -C project --board=sf32lb57-lcd-gyd7n6_epd_hcpu --board_search_path=../boards APPS=all PREINSTALL=weather,books STORAGE_IMAGE=1 -j8
```

强制恢复构建后重新构建了普通固件；最终目录内为普通固件，不是强制恢复固件。

| 项目 | DPI-HDK | LCD DevKit |
| --- | --- | --- |
| 构建目录 | `project/build_dpi-hdk_lb57gyd7n6_epd_hcpu/` | `project/build_sf32lb57-lcd-gyd7n6_epd_hcpu/` |
| 固件兼容标识 | `c3ef3cb0c5ea781138521e4a3d7653ee49b5cfeb0610f196f86154284f0e1161` | `2a89e1aacac4f2bf226344ca3db2cfebb6a15e2c80e0f5b403c7848c6b943f4a` |
| 主程序 ROM 使用 | 2,082,942 / 2,097,152 字节 | 2,082,206 / 2,097,152 字节 |
| 内置资源 ROM2 使用 | 5,635,054 / 8,388,608 字节 | 5,635,054 / 8,388,608 字节 |
| 应用开发包 | `app-sdk/`，393 个头文件、559 个导出符号 | `app-sdk/`，393 个头文件、559 个导出符号 |

各目录包含 `main.elf`、`main.bin/`、配套启动镜像和下载脚本、`fs_root.bin`、`app-resources/`、`debug-artifacts/`、导出的 `app-sdk/` 及 `app-catalogue.json`。六个应用为 weather、books、gallery、words、weread、board。books/gallery 版本分别为 10/9；两者数据版本仍为 1。各应用包均由对应固件重新编译并检查导入符号。

发布目录使用 `python tools/release_catalog.py --firmware <构建目录> --apps weather,books,gallery,words,weread,board` 生成，两板均返回六个包校验通过。生成时构建已结束；没有上传应用包或部署在线目录。

文件系统镜像预装 weather 和 books，生成脚本包含 `fs_root.bin@0x12A84000`；完整 words 词库保留在应用包中，没有塞入预装镜像。烧录文件系统镜像会覆盖内部文件系统，应按工厂初始化流程使用。

主程序分区剩余空间分别为 14,210 和 14,946 字节，后续增加固件常驻能力需要继续检查链接容量；以上是静态链接占用，不是运行时空闲堆。构建仍包含既有 RWX 段及 libgcc/libm 的 wchar_t 属性警告，未将构建结果表述为零警告。

独立开发包使用两板最终产物导出，再将 `EPD_APP_SDK_BUNDLES` 指向两目录运行测试。三个示例为 hello、netdemo、cfgdemo；测试复制到带空格的工程外目录、清除原工程环境变量后构建，并检查错误固件标识和未导出符号会被拒绝。

## 尚需设备验证

1. 分别使用正确的 DPI/LCD 板型，检查天气和书架预装入口、六个应用的安装与进入返回。
2. 从文件管理打开嵌套路径下的 TXT、Markdown、EPUB、MOBI、PNG 和 JPEG；应用已经打开或在后台时再次打开另一文件，核对返回关系与旧内容释放。
3. 在扫描、解码、网络请求及文件交接待执行时移除 TF 卡，检查旧卡请求被拒绝，页面与后台收尾后才能加载新卡。
4. 使用 `recovery on` 后重启，确认进入内置设置、动态应用不启动、安装卸载仍可用；执行 `recovery off` 后重启恢复正常。强制恢复固件须单独验证，刷写时保留文件系统。
5. 看板加载包内模板及用户覆盖模板；相同摘要不重绘，天气变化后只更新对应文字，退出看板后停止其页面定时器。
6. 安装工程外编译的三个示例，验证多页返回、网络事件、本地配置保存及停止清理。
7. 连续进入返回和插拔后查看 RAM0/RAM1，检查是否有持续增长；同时确认切页全刷、同页周期和状态栏免计数策略不变。

主机测试和编译不等于上述设备行为已通过。桌面预览复用几何布局，但浏览器字体栅格与换行不等同于 LVGL。现有阅读格式测试中的长 HTML/libxml2 工作集问题不属于本次修改，本记录不宣称全工程全部历史测试通过。
