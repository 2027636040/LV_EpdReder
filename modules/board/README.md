# 内容看板

`board` 是配置驱动的动态应用。安装包中的 `res/layout.json` 定义页面标题、文本卡片、服务摘要来源和网格布局。默认模板包含今日重点、便笺、服务摘要及阅读摘记；`res/templates/reading.json` 提供单列阅读组合。

页面使用平台 `epd_app` 控件，支持返回操作。文本卡片直接显示配置内容；摘要卡片通过 `epd_app_summary_read()` 取得平台保存的副本。服务未安装、正在停止或摘要为空时，卡片显示 `empty` 文本。看板本身不启动摘要服务。

## 配置

应用优先读取 `storage_app_path("board", STORAGE_APP_DATA, "layout.json")` 对应的用户配置。用户配置不存在时读取 `STORAGE_APP_CODE` 下的 `res/layout.json`。两条路径均跟随应用安装卷，内部存储和 TF 卡安装遵循相同规则。存在但无效的用户配置会显示错误，不会被默认配置悄悄替换。重新进入应用可加载修改后的配置。

字段、布局规则和配置示例见 [看板模板指南](../../docs/BOARD_TEMPLATES.md)。

## PC 预览与测试

在仓库根目录执行以下命令。Windows 使用 Visual Studio 的 x64 Native Tools 命令行；Linux 可使用 GCC 或 Clang。Python 脚本仅依赖标准库，C 编译器可通过 `CC` 或 `--cc` 指定。

```text
python tools/preview_board.py
python tools/preview_board.py modules/board/res/templates/reading.json --output modules/board/reading-preview.svg
python tools/preview_board.py --data path/to/layout.json --summary "weather=晴，适合散步"
python tools/tests/test_board_preview.py -v
```

默认输出为 [preview.svg](preview.svg)，可用支持 SVG `foreignObject` 的浏览器打开。PC 程序编译实际的 `board_model.c` 与 `SiFli-SDK/external/cJSON-1.7.17/cJSON.c`，SVG 消费纯 C 核心输出的绘制命令。卡片位置、尺寸及文本框边界与设备同源；浏览器的字体、文字换行和栅格化效果不等于设备 LVGL 输出。

## 构建和打包

激活仓库 SDK 的 `export.ps1` 后，使用已构建的平台配置编译应用：

```text
scons -C modules/board FIRMWARE=build_dpi-hdk_lb57gyd7n6_epd_hcpu -j8
scons -C modules APPS=board FIRMWARE=build_dpi-hdk_lb57gyd7n6_epd_hcpu -j8
```

第一条命令生成 `modules/board/output/board.so`，第二条命令通过 `tools/native_apps.py` 注册的 `add_board_package` 打包。更换 `FIRMWARE` 可选择已有的另一种板型构建目录。资源递归来自 `modules/board/res`；图标使用平台默认图标。模型和应用源码均在本模块内，无共享源码目录依赖。

平台需要导出 `app_service.h` 中的 `epd_app_summary_revision()`、`epd_app_summary_read()`，以及用于读取活动屏幕实际尺寸的 `lv_obj_get_width()`、`lv_obj_get_height()`。页面生命周期、内存所有权和验证边界见 [DESIGN.md](DESIGN.md)。
