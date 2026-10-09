# 应用开发

## 组织方式

内置功能位于 `src/ui/apps/`；天气和阅读动态应用分别位于 `modules/weather/`、`modules/books/`。
应用页面使用 SDK `gui_app_fwk.h` 和 LVGL V9。内置应用通过 `BUILTIN_APP_EXPORT` 注册，
动态应用通过安装包清单、`EPD_APP_DEFINE` 和 `app_main` 接入。Launcher 枚举注册表及安装目录显示入口；
新增独立应用不需要修改 `launcher.c`、`ui_model.h` 或现有页面路由表。

单文件应用可以放在 `src/ui/apps/app_notes.c`。多文件应用可以放在
`src/ui/apps/notes/`，用自己的 `SConscript` 定义编译组和私有头文件路径。

`apps/ui_pages.c`、`ui_internal.h` 和 `ui_navigation.c` 服务于已有参考设计页面。
它们管理主界面、文件管理和系统设置；独立应用直接拥有自己的页面数据。
阅读模块通过 `epd_app_open_text_settings()` 使用平台文本设置页，不引用 `ui_internal.h` 或内置页面 ID。
阅读模块的构建、安装和数据兼容说明见 [阅读应用](../modules/books/README.md)。
单词模块的学习、词库准备和安装说明见 [单词应用](../modules/words/README.md)。

## 最小页面入口

下面的页面使用触摸返回，不依赖方向键或确认键。图标使用已有相册图标作为示例。

```c
#include "gui_app_fwk.h"
#include "ui_font.h"
#include "icons/ui_icons.h"

static void back_clicked(lv_event_t *event)
{
    (void)event;
    gui_app_goback();
}

static void page_message(gui_app_msg_type_t message, void *parameter)
{
    (void)parameter;
    if (message != GUI_APP_MSG_ONSTART) return;
    gui_app_close_anim();
    lv_obj_t *screen = lv_screen_active();
    lv_obj_remove_style_all(screen);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(screen, ui_font_body(), 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "笔记");
    lv_obj_set_pos(title, 104, 103);

    lv_obj_t *back = lv_obj_create(screen);
    lv_obj_remove_style_all(back);
    lv_obj_remove_flag(back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(back, 32, 94);
    lv_obj_set_size(back, 56, 56);
    lv_obj_add_event_cb(back, back_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *image = lv_image_create(back);
    lv_image_set_src(image, &ui_icon_back);
    lv_obj_center(image);
}

static int app_main(intent_t intent)
{
    (void)intent;
    return gui_app_regist_msg_handler_ext("notes", page_message, NULL, 0);
}
BUILTIN_APP_EXPORT("笔记", &ui_icon_album, "notes", app_main, 1);
```

应用 ID 必须唯一，长度小于 SDK 的 `GUI_APP_ID_MAX_LEN`，不包含空格。
显示名称受 `GUI_APP_NAME_MAX_LEN` 限制，中文也按 UTF-8 字节数计算。
每个 C 文件使用一次相同组号的注册宏；同一个应用的子页面不再使用应用注册宏。

重新编译并烧录后，新入口自动追加到 Launcher 应用列表，不需要增加硬件按键协议。

## 页面和资源生命周期

- 根页调用 `gui_app_regist_msg_handler_ext()` 注册，根页名称由 SDK 固定为 `root`。
- 子页使用 `gui_app_create_page_for_app_ext()`，参数中的应用 ID 指向所属应用。
- 需要页面私有内存时传入结构体大小，在回调中用 `gui_app_this_page_memory()` 取得。
  SDK 分配并清零这块内存，页面停止后释放；指针不能保存在长寿命后台服务中。
- `ONSTART` 创建子控件；`ONRESUME` 读取最新快照并只更新变化字段。
- `ONPAUSE` 停止页面专属定时器/订阅并保存需要恢复的数据；
  `ONSTOP` 释放页面自己申请的资源。框架 screen 和页面内存不能重复释放。
- 普通返回使用 `gui_app_goback()`；回到应用内指定页面使用
  `gui_app_goback_to_page()`；退出整个应用使用 `gui_app_self_exit()`。
- 触摸事件中提交框架异步导航请求即可，不在仍使用事件目标对象时同步销毁页面。
- 后台服务发布受锁保护的数据快照，在 LVGL 所在线程读取。后台线程不访问页面对象。

## 全局输入

开发板按键由平台注册为唯一的 LVGL V9 键盘设备。KEY1/KEY3 选择前后控件，
KEY2 激活当前控件，长按 KEY1 返回。应用不注册硬件回调，也不维护物理按键队列。

使用 `platform/epd_app.h` 的 `epd_app_button()`、`epd_app_back_button()` 创建控件时，
控件自动加入最近的公共输入组；没有输入组时自动为所属 screen 创建。
自定义控件通过 `lv_group_add_obj(epd_app_input_group(screen), object)` 加入，
加入顺序就是选择顺序。隐藏和禁用控件不会被方向操作选中。

平台在处理输入时，依据 LVGL 当前 screen 选择输入组，不额外维护导航栈。
同一 screen 内的弹窗先调用 `epd_app_input_group(popup)` 创建独立输入组，
再创建按钮；`epd_app_input_back(popup, cancel_button)` 指定返回操作触发的按钮。
隐藏或禁用该按钮会阻止返回，适用于正在同步的弹窗。弹窗删除后，输入自动回到页面组，
页面可用 `lv_group_focus_obj()` 恢复打开弹窗之前的选中项。天气模块提供完整用法。

输入组由 owner 对象的 DELETE 事件释放，应用不能再手动调用 `lv_group_delete()`。
页面退到后台时保留组和焦点，但不接收按键；动态模块不需要在 ONRESUME/ONPAUSE 中绑定或解除硬件输入。
在当前页面中，仅阅读翻页等特殊语义需要专门处理；普通按钮继续使用与触摸相同的点击回调。

按键回调只入队，控件操作全部在 UI 线程执行。连续方向操作合并后仅改变最终焦点，
确认和返回分隔队列；触发导航后在控件事件返回之后执行框架调度，再处理后续按键。
没有输入时不运行键盘读取定时器，也不产生额外界面重绘。

## 墨水屏约定

全局转场配置为 `APP_TRANS_ANIMATION_NONE`，页面同时关闭自身转场。
界面使用静态白底，不启动渐变、滚动动画、光标闪烁或周期性重绘。
数据变化时比较新旧内容，只更新需要变化的控件；页面内数据更新不重建整个 screen。
应用不接管面板波形、扫描时序、供电控制或灰度差分策略。

## 本地动态应用

内部 NOR 使用 LittleFS，TF 使用 FatFs。应用可以通过 `epd_app.h` 引入通用存储 API：
`storage_app_path()` 按 ID 解析安装位置，并将程序、数据和缓存绑定到同一存储卷；`storage_mkdirs()` 支持两种文件系统；
`storage_record_save/load()` 保存带版本和 CRC 的小型业务记录。
目录和记录接口在内部使用存储锁，应用不自行操作分区或 Flash 扇区。
应用卸载默认保留其数据目录，清理程序和可重建缓存。

| 安装位置 | 程序及资源 | 用户数据 | 可重建缓存 |
| --- | --- | --- | --- |
| 内部存储 | `/flash/apps/<id>` | `/flash/data/<id>` | `/flash/cache/<id>` |
| TF 卡 | `/sdcard/.epd/apps/<id>` | `/sdcard/.epd/data/<id>` | `/sdcard/.epd/cache/<id>` |

系统字体、网络选择和设备设置仍由系统保存到内部存储，不随某个应用迁移。
这些目录是职责约定，不是原生模块的安全沙箱。
分区升级和普通下载的区别见 [存储迁移](STORAGE_MIGRATION.md)。

TF 卡挂载在 `/sdcard`，内部文件系统挂载在 `/flash`。待安装的应用放在 TF 卡的
`apps` 目录。首次安装时选择“内部存储”或“TF 卡”，分别安装到 `/flash/apps/<id>` 和
`/sdcard/.epd/apps/<id>`；更新沿用原安装位置。每个安装包使用独立子目录：

```text
/sdcard/apps/hello/
    app.json
    files.sha256
    hello.so
```

安装源目录与 TF 卡运行目录分离，复制安装包本身不会增加桌面入口。安装器在目标卷上暂存新包、
校验后发布，空间检查也针对目标卷。卸载列表显示应用所在存储；卸载保留对应卷的用户数据。
需要改变安装位置时，先卸载再安装并选择新位置。同一 ID 不同时安装到两处；若电脑手工复制出重复目录，
平台优先使用内部安装，卸载列表仍允许分别清理两份目录。

目标没有数据且另一卷只遗留同名应用数据、没有同名安装时，安装器在应用停止后复制、
核对并发布数据目录，成功后清理源数据。目标已有数据时以目标为准，不合并或覆盖。
已有卡内应用原先存到内部的数据，在确认加载卡内应用后按同样规则迁移。
缓存可以重新生成，不跨卷复制。迁移中断保留源数据或已发布的数据副本，重新加载时继续恢复；
迁移未完成时不启动相应卷的动态应用。

程序、私有解码库、入口图标及随包资源跟随安装位置。应用读取随包资源时使用
`storage_app_path(..., STORAGE_APP_CODE, relative)`，不拼接 `/flash/apps`。
该调用在找不到安装位置或原存储会话失效时返回 false，并清空输出路径。
业务记录使用 `STORAGE_APP_DATA`，缓存使用 `STORAGE_APP_CACHE`，不保存写死的内部目录。
模块装载前固定安装卷及插卡会话，后台服务继承该绑定；运行中不会改用另一卷的同名数据。
每次新任务检查 `epd_service_cancelled()`，分块 I/O 检查介质状态；缓存过的路径不能绕过这些检查。
原始文件替换使用 `storage_file_commit()`，读取前使用 `storage_file_recover()`；
`storage_file_replace()` 和记录接口已封装这些步骤。FAT 替换使用临时文件、旧文件备份与同卷重命名，
不能称为 LittleFS 式的原子覆盖，也不能保证物理拔卡时 FAT 元数据不损坏。

TF 卡移除时立即失效旧会话并禁止新访问，然后在 UI 线程退出动态页面、取消受管后台任务，
等待文件、网络订阅、在途命令和页面回调释放后再卸载文件系统。Launcher 同时释放卡内图标。
当前会统一停止所有动态应用，因为内部应用也可能读取 TF 文件；释放完成后内部应用可以重新运行。
35 秒仍未退出时继续保留模块引用，TF 卡保持禁用；在 UI 回调能够返回的前提下，系统内置页面恢复可操作。不强杀线程，
不卸载正在执行的代码，也不挂载替换卡。执行者结束后继续完成释放。
SD 控制器初始化/移除等待超过 10 秒时禁用 TF 卡至重启，不重复向未完成的控制器请求插拔。
当前 SDK 的 FAT 文件关闭失败时可能保留文件句柄。检测到遗留句柄或卸载失败时，不强制回收文件系统，
也不挂载替换卡；若原调用者无法释放句柄，需要重启恢复。应用共享地址空间，此机制不能隔离任意原生模块的死循环或非法访问。

插卡后仅扫描元数据并恢复安装事务。发现卡内应用时显示“是否加载 TF 卡内应用？”，
确认后完成数据迁移，发布入口并启动声明的后台服务；不会自动打开所有应用页面。
取消不影响文件管理或普通文件访问，同一次插卡不重复提示。
“设置 → 应用管理 → 加载 TF 卡应用”可以重新发起确认；拔卡后确认失效，换卡需要重新确认。
未经确认不运行卡内模块初始化或后台业务。直接复制旧目录包不绕过固件配置与包完整性校验。

源码目录的 `app.json` 填写应用元数据：

```json
{
  "abi": 1,
  "id": "hello",
  "version": 2,
  "data_version": 1,
  "name": "触摸示例",
  "background": true
}
```

安装包中的清单由 `tools/package_app.py` 生成，读取缓冲按文件实际大小申请；另外包含包格式版本、
配套固件的 `build_id`、文件索引摘要和 ELF 装载跨度。不要把源码清单直接复制到 TF 卡。
完整格式与后续存储规划见 [应用平台与存储实施方案](APPLICATION_PLATFORM_PLAN.md)。

目录名就是应用 ID，程序文件名必须为同名 `.so`。当前固件的 `RT_NAME_MAX=8`，
因此动态应用 ID 使用 1～7 个小写英文字母、数字或下划线；不得与内置应用 ID 重复。
显示名称最多 47 个 UTF-8 字节。没有独立图标时使用固件内的相册图标。

Launcher 在创建和恢复前台时扫描清单，读取名称且程序文件存在时显示入口。
入口图标从清单 `icon` 指定的独立 EZIP 文件读取，只加载当前桌面页，不装载应用程序。
图标缺失或无法读取时使用公共默认图标。安装完成事件由 UI 线程消费并更新目录。
旧格式和不兼容应用保留管理入口，便于卸载；安装和启动时执行严格的兼容性检查。
已安装的书架和天气、文件管理、设置保持顺序，新增入口按 ID 排序；每页两列四行，超过八个时显示分页圆点。
在主界面左右滑动切换应用页，切换不使用动画，首末页不循环。
可见应用目录与分页圆点按实际数量增长，不设固定应用数量上限；目录扩容失败时保留上次完整快照，并记录申请失败日志。

点击入口时，SDK 先查找内置应用；未命中时调用项目注册的装载回调。
回调先检查包格式、固件配置标识、文件索引和程序摘要、ARM ELF 布局，再调用 SDK `dlopen()`。
SDK 读取程序、重定位并加载至内存，项目随后校验应用描述并返回 `app_main`。
应用开始运行、页面切换、暂停、恢复、停止均由同一个 app_fwk 调度。
退出根页后，框架先停止页面、删除 screen 和页面上下文，随后调用卸载回调及 `dlclose()`。
声明后台任务的模块仍由服务管理器持有额外引用；页面退出不停止后台线程。
装载失败会留在主界面并提示打开失败，串口输出失败阶段。

动态应用是受信任的本地原生代码，不做签名校验，不提供权限隔离，也不自动下载程序。
模块不是通用 Linux/Android 共享库，必须使用配套固件的 ARM 工具链、SDK、LVGL 和编译配置构建。

### 模块接口

一个应用可携带多个私有解码库，放在安装包的 `codecs/<name>.so`，不设固定库数量上限。
使用打包工具的 `--library` 参数加入库文件；生成的 `libraries` 清单记录各库的装载跨度，
文件摘要覆盖所有库，安装时校验其 ARM ELF 布局。库名也限制为 1～7 个字符；
SDK 按库文件名识别已装载模块，不能用不同目录区分同名库。

已运行的应用在 UI 线程调用 `epd_app_library_open(app_id, name)`，通过
`epd_app_library_symbol()` 获取带版本和大小的接口表。工作线程通过该接口表使用解码器；
退出时先取消工作并释放库所属结果，再在 UI 线程调用 `epd_app_library_close()`。
打开时校验固件标识、程序及目标库摘要，并为装载峰值回收可淘汰的阅读字形缓存。
私有库与主模块共用应用的安装卷和卡会话，分别维护模块引用，不创建新的桌面入口。
书架的完整例子位于 `modules/books/codecs/`。

模块包含 `platform/epd_app.h`，导出两个符号：

```c
EPD_APP_DEFINE("hello");

int app_main(intent_t intent)
{
    (void)intent;
    return gui_app_regist_msg_handler_ext("hello", page_message, NULL, sizeof(page_data_t));
}
```

模块不使用 `BUILTIN_APP_EXPORT`。根页、子页和触摸返回直接调用 SDK 接口；
`EPD_APP_DEFINE` 同时定义应用描述和编译时配置标识，打包工具拒绝把旧模块与新配置拼成安装包。
`epd_app_font()` 提供平台字体，`epd_app_back_button()` 创建统一的触摸返回图标。
`epd_app_font_role()`、通用控件和状态栏接口可复用现有 UI 风格。
可供模块使用的额外 SDK/LVGL 导出集中在 `src/ui/platform/epd_app.c`，
TLS、JSON、解压等公共库导出集中在 `app_runtime.c`；不导出天气私有业务 API。
SDK 本身的 RTM 导出也可用于符号解析。模块新增外部函数依赖时，需要确认固件已导出该函数。

`EPD_APP_ABI` 表示项目侧接口版本，清单和模块描述的版本都必须一致。
SDK、LVGL 配置或公开结构体改变后，应配套重新构建模块；该版本号不是自动二进制兼容证明。
固件构建生成 `app-profile.json` 和 `epd_app_profile.h`。标识覆盖 SDK 及子模块版本/本地差异、
最终配置、工具链参数和项目公开接口；SDK 自动生成的证书数据不参与接口标识。

页面回调和触摸事件运行在 UI 线程。使用 SDK 页面内存保存控件和临时状态；
后台服务仍通过快照向 UI 发布数据。页面停止前解除自己的订阅、删除自己的 LVGL 定时器，
并结束页面专属的异步任务；SDK 管理的 screen 和页面内存由 SDK 释放。
长期任务使用 `epd_app_background` 描述符，生命周期与页面分离，停止协议见
[后台生命周期与网络](APPLICATION_LIFECYCLE.md)。
应用卸载后，其他组件不能继续持有模块函数指针、模块内字符串或资源地址。
模块的 `module_init` 不创建页面；页面注册放在 app_fwk 调用的 `app_main` 中。

### 编译示例

主工程默认只构建平台。已有配套固件时，在仓库根目录执行
`scons -C modules APPS=weather,books -j8` 可批量编译选中的应用并生成完整安装包，
统一位于 `project/build_<board>/app-resources/<应用ID>/`。
`APPS=all` 选择天气、书架、图库和单词四个正式应用；仅选择单词应用时需要另备词库、Node.js 与 `sharp`。
应用源码或资源变化后，再次选择该应用构建即可更新安装包。
平台和应用一起编译、可选出厂预装等用法见 [平台与应用构建](BUILDING.md)。

`modules/hello/` 提供独立触摸应用：点击计数、打开子页面、返回后保留计数，
退出应用后重新打开则重新初始化。控件同时支持触摸和平台全局按键，不包含硬件按键分发。
示例另有仅阻塞等待网络事件的受管后台任务，用于验证页面退出后保留模块、
更新和卸载时停止任务；没有动画或后台轮询。

在仓库根目录使用 PowerShell：

```powershell
. .\SiFli-SDK\export.ps1
Set-Location project
scons --board=dpi-hdk_lb57gyd7n6_epd_hcpu -j8
Set-Location ..\modules\hello
scons -j8
python ..\..\tools\package_app.py --manifest app.json --module output/hello.so --profile ../../project/build_dpi-hdk_lb57gyd7n6_epd_hcpu/app-profile.json --output output/package/hello
```

模块构建复用 SDK 的 `ua.BuildLibrary`，从固件的 `rtconfig.h`、`cconfig.h`
和该固件构建目录内的 `rtua.py` 读取配置与头文件路径。平台构建自动生成这些文件，不进入版本管理。
更换板级构建目录时使用 `scons FIRMWARE=build_<board> -j8`，
并先完成对应平台构建。各板级目录分别保存自己的 `rtua.py`，不共用最后一次构建的头文件路径。

产物为 `modules/hello/output/hello.so`，调试版本为同目录的 `hello.so.nostrip`。
把 `output/package/hello` 整个目录放入 TF 卡的 `apps/hello/`。
打包输出目录必须不存在；保留旧包时为下一次打包指定另一个输出目录。
私有资源通过 `--resources <目录>` 放入包内 `res/`，独立 EZIP 图标通过 `--icon <文件>` 放入 `icon.ezip`。
Launcher 已接入独立图标文件；没有图标的示例使用默认图标。
打开“设置 → 应用管理 → 应用安装”，选择“触摸示例”并确认。
安装完成返回主页后，在应用列表中打开“触摸示例”；
超过一页时左右滑动查找入口。

卸载通过“设置 → 应用管理 → 应用卸载”完成。更新在“应用安装”选择同 ID 的较高版本，
不需要先卸载；相同版本仅允许为不同固件配置重新构建。数据格式版本变化暂不自动迁移。
卸载会删除 `/flash/apps/<id>` 中的程序和资源，保留 `/flash/data/<id>` 的持久化数据；内置应用不出现在卸载列表中。
返回主页后入口列表重新读取，不需要重新烧录主固件。

安装器在工作线程中检查兼容配置、文件索引、ARM ELF 格式和实际可用容量，
以 4 KiB 缓冲分块复制目录，并重新计算临时目录内各文件的 SHA-256 后才发布。
支持随包安装资源子目录，不设固定负载文件数量上限；相对文件路径最多 159 个 UTF-8 字节、八层，
完整路径小于 256 字节。摘要流式计算，不把整个资源库读入 RAM。
复制期间页面只显示静态提示，完成结果由 UI 线程处理。
内置应用的独立资源目录包使用 `kind: "resources"`，不包含 `.so`，不新增桌面入口。
天气现已使用完整模块包；旧资源包只保留升级兼容，部署见 [天气应用资源](WEATHER_RESOURCES.md)。
安装器在停止页面和后台任务后，以阶段日志和目录改名发布更新；不会自动格式化未挂载的存储。
当前内部文件系统分区为 5,616 KiB，容量信息显示的是文件系统可管理空间，不包含固件和波形分区。

### 完整天气参考应用

`modules/weather` 将页面、设置、业务、持久化与常驻任务组合成一个包，通过 `APPS=weather` 显式构建配套包。
它通过 `epd_service_publish_summary()` 发布平台持有的摘要副本，主页不保存模块函数指针；
可选 `command` 回调由服务管理器保护调用寿命。页面关闭不停止后台，卸载等待后台和在途命令结束。
完整源码职责和部署见 [天气模块说明](../modules/weather/README.md)。

### TF 卡热插拔

内部文件系统使用 `/flash`，TF 卡使用 `/sdcard`。当前 57 DPI 墨水屏板使用 PA11
低电平检测插卡，GPIO 双边沿中断唤醒存储线程，稳定 25 ms 后处理插拔，不周期扫描目录。
拔卡时先禁止新的 TF 访问，通过 app_fwk 停止动态应用页面及受管后台任务，包括可能读取 TF 文件的内部应用。
释放结束后，内部安装的书架应用可以重新打开内部书籍。安装线程结束复制并关闭文件后，存储线程卸载文件系统、通知 SDK 移除卡设备。
插卡后使用 SDK 重新识别卡设备，再挂载文件系统；前台文件列表、书架和容量页按变更更新。

动态应用的 ONSTOP 必须结束页面存储任务并关闭页面文件、目录和文件字体。
跨页面的存储任务由受管后台线程在收到停止请求时关闭，返回后才能卸载模块。短时文件操作使用
`storage_lock()` / `storage_unlock()` 包住，并在锁内检查 `storage_path_available(path)`；
这些接口声明在 `storage.h`，已经导出给模块。不要持有存储锁等待 UI 回调。
检测到遗留句柄时不会强制释放
其文件系统；串口报告移除受阻，关闭占用者后重新插拔再试。SDK 关闭失败而遗留的句柄可能需要重启才能清除。

写入过程中直接拔卡可能丢失尚未落盘的数据。安装复制未完整完成时不发布应用目录；
阅读恢复以已经保存的进度为准。需要上板检查空卡槽启动后插卡、阅读时拔卡、复制时拔卡，
以及快速拔出再插入不同卡的情况。

### 应用设置入口

内置应用可以通过 `platform/app_settings_registry.h` 注册自己的设置入口。
描述符及其字符串使用静态存储，启动初始化时调用 `app_settings_register()`。
已有参考页面填写对应页面 ID；独立应用填写 `UI_PAGE_COUNT`，框架会启动该应用并传入
`page=settings` 的 Intent。设置列表不需要新增应用专属分支。

动态应用在 `app.json` 中增加 `"settings": true`，便会出现在“应用管理 → 应用设置”中。
应用在 `app_main()` 中通过 `intent_get_string(intent, "page")` 判断是否进入设置页面，
设置页面的创建、返回、资源释放仍使用 app_fwk。不要保存传入的 Intent 指针供异步使用。

### SDK 配置

| 配置 | 用途 |
|---|---|
| `RT_USING_MODULE=y` | 使用 SDK 的 ELF 动态库装载、符号解析和卸载 |
| `RT_MODULE_MEM_CUSTOM=y` | 使用 SDK 的 `app_cache_alloc(CACHE_PSRAM)` 分配模块内存 |
| `RT_USING_XIP_MODULE=y` | 满足固定 SDK 中 `dlelf.c` 对 XIP 辅助函数的编译依赖 |
| `APP_TRANS_ANIMATION_NONE=y` | 禁用应用转场动画 |
| `GUI_MAX_RUNNING_APPS=2` | 保留主界面与当前功能应用 |

实际装载使用 `dlopen()` 的内存运行路径，不调用 `dlrun()` 或 Flash 安装接口，
不启用 solution 的 `DL_APP_SUPPORT` 注册文件机制和 `mod_installer`。
PSRAM 不足时，SDK 分配器可能回退到系统堆；模块内存与现有应用缓存共享。
SDK 在装载完成后负责数据缓存写回和指令缓存失效。

### 内存分配和用量

通用分配接口位于 `src/platform/epd_memory.h`，`platform/epd_app.h` 包含该接口。
服务和底层组件直接使用公共头文件，不依赖 LVGL。

TLS 通过项目的 `MBEDTLS_USER_CONFIG_FILE` 启用 mbedTLS 标准内存分配接口。
SDK 初始化共享 PSRAM 堆后，平台在组件初始化阶段一次性注册分配和释放函数。
证书解析、TLS 收发缓冲和密码运算工作内存使用 RAM1，释放后归还共享堆；
申请失败直接交给 TLS 错误处理，不回退占用 RAM0。证书链及强制验签策略不变。
动态应用使用主程序的 TLS 实现，不自行修改全局分配器。
固件与模块都使用生成的 `rtconfig.h` 和相同项目 TLS 配置，配置改变后需更新配套应用包。

动态模块的代码、全局数据和 BSS 在 `dlopen()` 时分配，最后一个引用由 `dlclose()`
释放。页面退出不等于模块卸载：受管后台服务持有自己的模块引用，服务结束后才释放。
应用可通过 `epd_app_alloc/epd_app_realloc` 选择 SRAM 或 PSRAM，使用 `epd_app_free`
释放；PSRAM 请求不会回退到 SRAM。公共 LVGL 堆、屏幕缓冲区和共享缓存池的预留空间
不会随着某个应用退出而撤销，池内释放的块可以继续被其他使用者分配。

“设置 → 内存使用”将 HCPU SRAM 标为 RAM0、PSRAM 标为 RAM1。当前板的物理容量分别
为 512 KiB 和 16 MiB。进入页面或点击“刷新”时读取一次快照，没有刷新定时器。
每个 RAM 的口径为：总量 = 已用 + 可分配 + 未分配。已用包含静态数据、线程栈、
帧缓冲、SDK 保留区域、分配器元数据和动态分配的内存，不再划分到各个应用。
可分配是已初始化堆的空闲块之和，不代表存在同等大小的连续块；
未纳入堆的 PSRAM 尾部单列为未分配。

快照直接读取系统堆、应用/图像共享 PSRAM 堆及 LVGL 堆的空闲量，
分别用分配器锁保护读取，不遍历堆块、不枚举应用，也不分配统计数组。
后台线程可以在两次堆采样之间运行，因此快照不表示全系统同一时刻的状态。

### 上板检查

1. 复制并安装示例后确认入口出现；没有应用目录时，内置入口正常显示。
2. 打开示例，连续点击计数，进入子页再返回，确认次数保留。
3. 返回主页，检查串口出现 `app hello: page reference released`；后台继续等待网络，再次进入示例计数回到零。
4. 反复开关示例，确认没有内存持续增长、悬挂回调或原有页面返回异常。
5. 让天气后台更新或书籍后台分页与示例运行交错，返回原页面确认数据仍能更新。
6. 错误配置标识、旧格式清单或损坏的程序不能进入 `dlopen`；安装时修改任一资源文件，应校验失败。
7. 程序缺少依赖符号时，点击后能提示失败并继续操作主页；旧包仍可从应用管理卸载。
8. 更新或卸载 hello 时确认后台任务先停止；网络事件不会投递到已经释放的事件对象。
