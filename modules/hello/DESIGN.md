# 触摸示例应用设计

## 目的与职责

`hello.so` 是最小动态应用示例，用于说明入口声明、框架页面内存、子页返回和独立后台服务。示例提供点击计数和一个说明子页，不保存业务文件。当前[清单](app.json)版本为 2、数据版本为 1，声明 `background: true`。

全部应用逻辑位于 [hello.c](hello.c)。`EPD_APP_DEFINE("hello")` 声明应用身份，`app_main()` 在 UI 线程向 app_fwk 注册根页处理函数。公共控件、字体和返回按钮来自[平台应用接口](../../src/ui/platform/epd_app.h)。

## 设计意图与取舍

示例刻意把页面状态、模块服务和持久化三件事分开：计数跟随页面，网络等待跟随受管服务，没有文件保存。进入子页再返回保留计数，退出应用再进入从零开始，使开发者能看到框架保留页面与应用主动持久化的区别。

后台示例只阻塞等待事件，不轮询重绘或直接操作标签，用最小业务展示停止、订阅和模块引用的关系。`background: true` 是教学选择，不是所有应用必须常驻的规则。示例不加入默认正式应用列表；实际应用可以只采用需要的页面或后台部分。平台层取舍见[开发意图与设计决策](../../docs/DESIGN_INTENT.md)。

## 页面流程与状态所有权

根页 `ONSTART` 从 `gui_app_this_page_memory()` 取得 `hello_page_t`，创建计数标签、计数按钮和子页按钮。点击计数只增加本页的 `count` 并更新标签；打开子页调用 `gui_app_create_page_for_app_ext()`，返回沿用平台按钮和框架导航。

计数和标签指针属于根页上下文。进入子页时框架保留根页，因此返回后计数保留；退出整个应用后根页销毁，再次进入创建新上下文，计数从零开始。示例没有跨启动持久化，也没有模块全局计数来模拟页面恢复。

回调仅在 `ONSTART` 创建控件，没有页面定时器、页面专用线程或额外图像。screen、子控件及页面内存由框架销毁，因此没有自定义 `ONSTOP` 释放函数。控件事件在 UI 线程运行，后台不持有标签或其他控件指针。

## 后台生命周期与接口

`epd_app_background` 声明栈大小 2048 字节、优先级 24，并订阅网络事件。`background_run()` 使用 `epd_service_wait(..., RT_WAITING_FOREVER)` 阻塞等待：收到网络事件时复制 `network_snapshot_t` 并打印是否就绪及网络代次，收到停止事件或检测到取消后退出。

平台根据清单在应用安装和启动枚举时装载后台并持有模块引用，卡内应用需先确认加载。页面退出只释放页面引用，后台仍可等待网络；更新、卸载或存储退出时，平台先停止服务，等待线程返回后再释放模块。TF 卡移除统一停止动态应用，因此内部安装的 hello 也参与退出。示例不发起网络请求、不注册自己的硬件按键分发，也没有后台轮询。

应用导出入口和后台描述，不提供模块私有命令或跨应用业务 API。网络快照是即时副本，示例不保存可变网络对象或订阅回调指针。

## 构建与部署

hello 不在正式应用的 `APPS=all` 列表中，需要单独构建和打包。在仓库根目录执行：

```powershell
. .\SiFli-SDK\export.ps1
scons -C project --board=dpi-hdk_lb57gyd7n6_epd_hcpu -j8
scons -C modules/hello -j8
python tools/package_app.py --manifest modules/hello/app.json --module modules/hello/output/hello.so --profile project/build_dpi-hdk_lb57gyd7n6_epd_hcpu/app-profile.json --output modules/hello/output/package/hello
```

已有兼容平台时省略平台构建。打包输出目录需要不存在；保留已有包时为新包选择另一个目录。LCD 载板构建使用 `FIRMWARE=build_sf32lb57-lcd-gyd7n6_epd_hcpu`，打包同时选择对应的 `app-profile.json`。

将完整包放到 TF 卡 `apps/hello`，在“设置 → 应用管理 → 应用安装”安装“触摸示例”。没有独立图标时平台使用默认图标。源码 `app.json` 不直接作为成品清单使用，打包器补充兼容标识、文件摘要和装载信息。通用规则见[应用开发说明](../../docs/APPLICATION_DEVELOPMENT.md)及[构建说明](../../docs/BUILDING.md)。

## 验证入口与限制

设备检查沿业务流程进行：连续计数、进入子页再返回应保留次数；退出应用后重进应归零；关闭页面后网络事件仍可唤醒后台；更新或卸载时后台先结束，旧模块不再打印网络事件。

示例没有专用主机业务测试；构建和安装可验证入口、导入符号及包兼容性，不能单独证明板上导航和停止时序。hello 的范围是页面与后台生命周期示例，持久化、联网业务和复杂输入应参考相应正式应用设计。
