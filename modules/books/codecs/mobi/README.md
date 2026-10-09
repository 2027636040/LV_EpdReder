# MOBI 私有解码器

`output/bk_mobi.so` 仅导出 `book_converter`，使用 `book_codec.h` ABI 1。
模块内保留 `EPDAPP:<EPD_APP_BUILD_ID>` 标记。正文通过调用方的同步
`sink.markup` 转换，调用方须先建立公共 markup 的分配会话。

## 实现

- 使用 solution libmobi 0.12 的 DFS 延迟记录和
  `mobi_parse_rawml_opt(rawml, mobi, true, false, false)`。
- KF8 逐章重建并释放；KF7 逐记录解压。第一次遍历只收集跳转位置，第二次输出正文；
  不展开整书 RAWML。记录偏移缓存保存真实解压边界，处理不等长 Huffman 记录。
- 预扫描以最多 4 KiB 的连续块查找标签；UTF-8 正文也按连续块送入公共解析器，
  在标签及跳转锚点处停止。输出字节顺序、目录和链接目标不变。
- 按需读取共用一个只读文件句柄和 16 KiB PSRAM 预读缓冲，转换结束或取消时关闭并释放。
  记录查找从上次位置向前推进，回读时重新定位；HUFF/CDIC 字典在同一本书内只解析一次。
- 建立资源索引时只检查各记录前 8 字节，不预加载图片、字体或 SRCS 附加内容；
  真正使用图片时才读取图片完整记录。
- NCX 标题映射至 `mobi:<part>:<offset>`；KF8 fid/off 和 KF7 filepos 转换为同一锚点。
  正文命名锚点使用 `mobi-id:<part>:<id>`。
- 图片与 CSS 地址转换为模块内相对资源名，避免被公共解析器当成外部 URI。
  JPG/PNG/GIF/BMP 与未在正文引用的封面同步送入图片回调。资源关闭即释放。
- 全部动态内存经调用方分配器；分配记录表回收第三方错误路径遗留对象。
  每次 DFS open/read/lseek/close 都受 storage 锁保护，检查介质代次/切换状态；
  sink 回调不在锁内执行。取消、I/O、内存不足和不支持格式分别返回对应 BOOK 错误。

串口 `[mobi]` 记录头部解析、资源索引、链接扫描、正文转换及首批内容完成阶段。
`elapsed` 使用 RT-Thread tick 频率换算为毫秒；`prefetch_reads`、`bytes` 只统计
预读缓冲实际执行的 DFS 读取，不包含文件头解析时的读取。
`[document] first-publish` 表示首批缓存已同步并交给排版；`first-page-ready` 表示
目标页控件已准备好，不包含随后墨水屏发送波形的时间。

## 构建与验证

在工程 SDK 的 `export.ps1` 已加载后，于本目录运行：

```powershell
scons FIRMWARE=build_dpi-hdk_lb57gyd7n6_epd_hcpu -j8
./tests/check_modules.ps1
```

此命令只构建私有库。`FIRMWARE` 指向已有主固件配置，不会构建主固件。
`tests/host.rsp` 为 Windows MSVC/AddressSanitizer 夹具；
`tests/build_common.py` 为 Linux GCC/AddressSanitizer/UndefinedBehaviorSanitizer
与实际公共 markup 的联测夹具。测试产物仅位于本模块 `output`。

## 格式边界

支持本地 libmobi 的 MOBI/KF7/KF8 容器，不代表支持 KFX、Topaz 或 DRM 解密。
加密与 Print Replica 返回不支持。嵌入字体不加载，排版继续使用宿主字体。
样式范围由公共 markup 决定；复杂 SVG、音视频和脚本不在本模块支持范围内。
单标签缓冲为 8192 字节；超限返回不支持，HTML 注释可流式跳过。
章节和资源仍需要相应大小的动态内存；内存上限由传入分配器控制。

## 许可证与来源

供应商目录：`../../third_party/libmobi`，来源及补丁见该目录 `PORTING.md`。
安装包许可证原文：`COPYING`（LGPLv3）和 `COPYING.GPL3`（GPLv3）。
测试样本保留在供应商原有 `tests/samples`；这些样本不是运行时安装资源。
