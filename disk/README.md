# 内部 LittleFS 初始内容

平台编译时使用 SDK 自带 `mklfsimg.exe` 将此目录打包为 `fs_root.bin`，挂载位置为 `/flash`。
默认不预装动态应用；通过 `PREINSTALL=weather,books` 等列表选择需要叠加到 `apps/<id>` 的完整安装包，
不把生成文件写回此目录。普通构建生成但不下载该镜像；首次初始化烧录需要 `STORAGE_IMAGE=1`，旧设备迁移使用
`STORAGE_IMPORT=<备份目录>`。已有数据的设备不能用初始镜像覆盖文件系统。
已迁移设备通过 TF 的 `apps/<id>` 和应用安装入口添加应用，保留原文件。
构建命令见 [平台与应用构建](../docs/BUILDING.md)。

应用程序及资源放在 `apps/<id>`，应用数据放在 `data/<id>`，可重建缓存放在
`cache/<id>`，系统设置放在 `system`，内部字体放在 `fonts`，更新包放在 `update`。

当前书架扫描内部存储和 TF 卡根目录中的 TXT。阅读字体使用
`fonts/Song.ttf`、`fonts/Hei.ttf`、`fonts/Kai.ttf`、`fonts/Monospace.ttf`。
内部缺少所选字体时，先从 TF 卡对应路径导入，再从内部文件打开。
