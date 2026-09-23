# 天气图标

本目录保留 [SuperKey](https://github.com/SiFliSparks/SuperKey) 的
`app/asset/weather/` 中全部 70 个 PNG 原文件，文件名中的数字用于天气代码查询。
随附的 `LICENSE.SuperKey` 为来源仓库的授权文件。

`tools/generate_weather_icons.cjs` 将整套资源生成 160×160 和 48×48 的 LVGL v9
A8 图像。图像转换为 16 灰阶并保留透明边缘，显示时使用黑色着色，不使用运行时缩放。
原始 PNG 不被修改；固件直接读取资源分区中的常量，不依赖 TF 卡。

`ui_weather_icon(code, large)` 根据和风天气代码返回图像，未知或缺失代码返回 `999`。
目录中的 `800`–`807` 资源也全部保留并纳入索引。

安装 Node.js 的 `sharp` 模块后，在工程根目录运行：

```powershell
node tools/generate_weather_icons.cjs
```

生成文件已纳入工程，普通固件编译无需重新生成。
