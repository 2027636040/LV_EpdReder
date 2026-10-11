# 微信读书

`weread.so` 是独立动态应用。设备通过 HTTPS 请求微信读书网页接口，手机扫码确认登录后，在设备上读取书架、章节目录和正文。应用不依赖 `books.so`，也不需要手机代理服务器。

## 构建与安装

先按平台说明构建对应板型，再在仓库根目录执行：

```powershell
scons -C modules APPS=weread -j8
```

默认使用 DPI 板已生成的平台配置。使用 LCD 载板配置时指定对应构建目录：

```powershell
scons -C modules APPS=weread FIRMWARE=build_sf32lb57-lcd-gyd7n6_epd_hcpu -j8
```

安装包输出到对应的 `project/build_<board>/app-resources/weread/`。将完整目录放到设备可访问的文件系统中，通过设置中的应用安装入口安装。应用可以装在内部存储或 TF 卡，数据和缓存随安装位置保存。

也可以随平台一起构建或预装：

```powershell
scons -C project --board=dpi-hdk_lb57gyd7n6_epd_hcpu APPS=weread -j8
scons -C project --board=dpi-hdk_lb57gyd7n6_epd_hcpu PREINSTALL=weather,books,weread STORAGE_IMAGE=1 -j8
```

第二条命令生成包含预装应用的文件系统镜像。烧录 `fs_root.bin` 会覆盖内部文件系统原有内容，不适用于保留已有账号数据的日常应用升级。

## 使用

1. 连接可用网络，打开微信读书，使用微信扫描二维码并确认登录。如果手机显示“双重验证码”，在设备出现的数字输入界面填写四位验证码并点击“确认”。
2. 在书架选择图书，再从目录选择章节或继续上次阅读。
3. 点击正文左侧返回上一页，点击其他区域前进；阅读菜单可打开文本设置、切换章节或返回目录。
4. 书架和目录使用上一页、下一页浏览，没有固定图书数量或章节数量上限。实际可用数量由数据和分配结果决定。书目先显示，后台再为当前可见的八本书准备封面。
5. 已完整缓存的书架、目录和章节可以离线打开。退出账号会清除保存的登录会话；不同账号的阅读位置和缓存互相隔离。

阅读位置使用账号、bookId、chapterUid 和正文内的记录位置，而不是书架序号。调整字号后重新排版，位置仍按正文恢复；正文修订后不复用旧内容偏移。当前只保存设备本地进度，不上传云端进度。

双重验证复用原二维码的登录会话，等待输入时不继续轮询服务器。输入错误可重新填写；网络失败可再次提交；验证码过期或次数达到上限后，点击“登录”重新扫码。输入界面的“重新扫码”会替换原登录请求，返回或退出应用会取消未完成的登录。验证码仅在当前请求内使用，不写入会话文件，也不记录到日志。日志中的 `[weread] login result=... verify_wait=...` 只包含结果状态与是否仍等待验证码。

登录结果 `3` 表示需要验证码，`4` 表示验证码错误，`5` 表示验证码过期，`6` 表示验证次数超限，`0` 表示登录会话已建立。网络等错误沿用客户端负值错误码；`verify_wait=1` 表示仍保留本次验证，可以继续输入。

打开目录时优先联网获取更新信息，网络失败则使用完整目录缓存。章节缓存比较目录中的 `updateTime`，发生变化就重新获取正文；恢复位置还会比较正文校验值。服务器未提供更新时间时保留零值，客户端不会自行推测内容修订。已经打开的目录保持快照一致，重新进入目录后检查更新。

正文复用书架的结构化排版，支持段落、标题、强调、插图及章节内链接，不提供浏览器完整 CSS 能力。离线缺失图片时显示占位文字，缺失样式文件时使用默认排版；这些降级结果不会保存成完整的排版缓存，后续打开可重新获取资源。

## 代码边界

| 位置 | 职责 |
| --- | --- |
| `client/` | 登录、会话续期、Cookie、书架、目录与章节协议，独立于 UI 和文件系统 |
| `weread_service.c` | 平台受管工作线程、取消代次、不可变结果快照、后台位置保存 |
| `weread_store.c` | 安装卷中的会话、账号隔离缓存、正文版本和位置记录 |
| `weread_resources.c` | 章节插图下载与缓存；不向图片域名发送登录凭据 |
| `weread_covers.c` | 同一工作线程内的可见封面队列、灰阶缩放及 UI 像素所有权转移 |
| `weread_ui.c` | app_fwk 页面、登录二维码、书架目录和阅读菜单 |
| `../common/http/` | 天气与微信读书共用的 HTTPS 传输，保留证书校验、取消和网络切换检查 |
| `../common/reader/` | 共用排版与显示组件，按字体、行距和屏幕区域分页 |

公共组件以源码形式链接进各动态应用，各自拥有运行状态。平台仍是服务生命周期、应用装载、网络和存储的管理者。页面由 app_fwk 管理，不另建导航栈。后台线程不访问 LVGL 控件；页面关闭后，旧结果只释放资源，不再提交到 UI。

页面回调的第二个参数是应用 ID；导航传入的图书和章节参数通过 `gui_app_this_page_userdata()` 获取，页面自有状态通过 `gui_app_this_page_memory()` 获取。目录页保存章节选择参数，阅读页在初始化时取得目录快照引用，并在销毁时释放该引用。

加载中返回和拔卡会取消请求。HTTP 的套接字与 TLS 操作可取消；SDK 的同步 DNS 解析只能在返回后检查取消，不能承诺立即终止 DNS。停止流程等待工作线程和显示引用释放，不强制销毁仍在执行的线程。

## 数据与权限

接口约定参考 `epdiy-epub/src/weread` 源码，复用声明见 `LICENSE.reference`。双重验证码流程依据[微信读书官方阅读页脚本](https://cdn.weread.qq.com/web/wrwebnjlogic/js/app.89f39f59.js)的登录实现：使用 `otp` 继续 `/web/login/weblogin`，成功后完成 `/web/login/session/init`。这些网页接口不是本项目可控制的稳定公共 API，服务端变更可能需要更新客户端。应用只使用服务端正常提供给当前账号的内容；验证未完成、权限不足、片段校验错误不会作为有效章节缓存。

会话文件包含登录凭据，应按账号数据管理；应用不会在正常日志中打印 Cookie、访问令牌或刷新令牌。缓存插图只使用 HTTPS，不携带账号 Cookie 到第三方域名。

## 验证

主机测试入口放在 `tests/`，公共 HTTP 和公共阅读器也有独立测试。协议测试使用脱敏的合成响应，不等同于真实账号登录通过。

Linux/WSL 中可运行以下测试；使用宿主 C 编译器及地址、未定义行为检查器：

```sh
python3 modules/weread/client/tests/run.py
python3 modules/weread/tests/run_store_tests.py
python3 modules/weread/tests/service_run_tests.py
python3 modules/weread/tests/run_ui_tests.py
python3 modules/weread/tests/run_covers_tests.py
python3 modules/weread/tests/run_covers_raster_tests.py
python3 modules/common/http/tests/run_http_tests.py --sanitize
python3 modules/common/reader/tests/run.py --sanitize
python3 tests/run_reader_view_tests.py --sanitize
python3 tests/run_service_tests.py --sanitize
```

页面测试另需 CMake，编译本地 LVGL 和实际 `weread_ui.c`，模拟 app_fwk 的回调参数约定以及后台服务接口，检查导航、延迟创建、返回重进和图书信息分配失败。该测试不执行网络请求、正文排版或硬件刷新。

上板需检查：扫码成功与取消、退出后重进、跨目录分页、读到章尾切章、修改字号、离线读缓存、退出账号后切换账号、加载中返回，以及 TF 卡拔出后安全退出和重新加载。

实施清单、构建结果及尚未完成的实机验收见[验证记录](../../docs/WEREAD_IMPLEMENTATION_TODO.md)。
