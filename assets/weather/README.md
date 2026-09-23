# 天气图标

本目录保留 [SuperKey](https://github.com/SiFliSparks/SuperKey) 的
`app/asset/weather/` 中全部 70 个 PNG 原文件，文件名中的数字用于天气代码查询。
随附的 `LICENSE.SuperKey` 为来源仓库的授权文件。

`tools/generate_weather_icons.cjs` 将整套资源生成 160×160 和 48×48 的灰阶 PNG，
保存到 `assets/ezip/weather/`。前景统一为近黑色 `#111111`，原图明度不参与透明度计算；
缩放后的透明度量化为 16 级，保留透明背景和抗锯齿边缘。
不使用运行时缩放，原始 PNG 不被修改。
修改脚本中的 `FOREGROUND_GRAY` 可调整前景深浅，例如 `0x33` 对应深灰色 `#333333`。

`assets/SConscript` 使用 SDK 的 `Env.ImgResource()` 将 PNG 转换为 LVGL v9 EZIP
资源，并将数据和描述符链接到 ROM2。SF32LB57X 使用 2 KiB 压缩窗口。
固件通过 EZIP/EPIC 显示这些资源，不依赖 TF 卡或 LodePNG 软件解码器。

`ui_weather_icon(code, large)` 根据和风天气代码返回图像，未知或缺失代码返回 `999`。
目录中的 `800`–`807` 资源也全部保留并纳入索引。

安装 Node.js 的 `sharp` 模块后，在工程根目录运行：

```powershell
node tools/generate_weather_icons.cjs
```

生成的 PNG 和天气代码查询表随工程提供，普通固件编译不需要 Node.js。
EZIP 转换由 SCons 自动执行，输出保存在板卡构建目录中。
