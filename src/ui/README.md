# UI 层（ui）

> 本层由 **UI 同事**负责：LVGL v9 页面实现与交互框架。
> 数据全部来自 `src/services/`（只看头文件即可，勿深入实现）。

## 规划（依据需求文档）

- 页面栈 / 页面注册 / 生命周期 / 墨水屏刷新节流
- 按键（KEY1 上 / KEY2 确定·返回 / KEY3 下）与触摸输入
- 低功耗状态机（ACTIVE / STANDBY / DEEPSLEEP）
- 页面清单：锁屏 / 主页 Launcher / 书架 / 阅读器 / 阅读设置 /
  设置 / 文本设置 / 天气 / 城市选择 / 文件传输 / Wi-Fi 配网 / 关于 / 弹窗

## 已有内容

- `ui_events.h`：按键/触摸动作（`UIAction`）与 `ActionCallback_t`，
  由 `src/boards/controls/buttons.c` 回调分发。
- `SConscript`：本目录下新增的 `.c` 会自动进入构建；
  需要额外头文件路径时在自身 `SConscript` 中追加 `CPPPATH`。

## 与数据服务层（services）的接口

| 能力 | 头文件 | 关键接口 |
|---|---|---|
| 天气数据 | `weather.h` | `weather_request_refresh()` / `weather_get_info()` / `weather_set_event_cb()` / 城市接口 |
| 蓝牙 PAN | `bt_pan.h` | `btpan_get_state()` / `btpan_enable()` / `btpan_set_event_cb()` |

- 详细用法与线程约定：见 `src/services/README.md`
- 注意：**服务事件回调不在 UI 线程**，回调里请用 `lv_async_call()` 切回 UI 线程
  再操作 LVGL 对象。

## 构建

```powershell
. <SiFli-SDK>\export.ps1
cd project
scons --board=dpi-hdk_lb57gyd7n6_epd --board_search_path=.. -j8
```

屏幕规格：684 × 1216（竖屏）；墨水屏刷新模式与全刷周期控制见仓库根 `README.md`。
