# 结构化电子书转换器

`formats.h` 提供 Markdown、EPUB 2/3、分卷 EPUB 的同步流式转换，以及供私有 MOBI 后端调用的公共 HTML/XHTML 解析器。转换器只调用 `book_sink_t`，不创建 UI 对象。

## 接口与所有权

`book_format_detect(path, is_directory)` 按扩展名识别文件；目录要求存在 `basepackage.zip` 和 `chapter*.zip`。探测不解析全文。`book_format_name(format)` 返回显示名称。枚举也包含 TXT、MOBI，供主应用统一路由。MOBI 的值保持为 5，以兼容已有阅读记录。

`book_format_convert(path, sink, allocator, error, capacity)` 管理完整转换作用域，只处理 Markdown 和 EPUB（含分卷）。TXT 和私有模块由主应用另行路由。返回共享头中的 `BOOK_*`；sink 返回负值时终止转换并清理资源。

`book_markup_convert(sink, stream, base, resources)` 与 `sink->markup` 签名一致。私有模块使用公共解析器前，调用 `book_markup_begin(allocator)`；模块转换结束的所有路径调用 `book_markup_end()`。普通 `book_format_convert()` 自行管理 begin/end。所有转换在同一文档工作线程串行执行，嵌套 begin 会失败。end 清理私有 XML 状态并清除活动分配回调。

文本每片不超过 2048 字节，不截断 UTF-8 字符。sink 必须同步复制文本、目录和元信息。图片回调同步读取借用的流，不应关闭或保留流；转换器在回调返回后读取剩余数据、检查 ZIP 长度/CRC 并关闭。图片流不要求 seek。公共 markup 不关闭调用者提供的正文流。

`anchor(id, NULL, 0)` 定义当前正文位置；`anchor(id, title, level)` 声明目录项。EPUB nav/NCX 的目录声明可能早于正文位置定义，sink 应按 ID 合并。EPUB ID 使用 `资源路径#片段`，分卷正文 ID 使用 `chapterN.zip!资源路径#片段`。外部链接仅输出目标字符串，不访问网络。

## 文件与构建

| 文件 | 职责 |
| --- | --- |
| `formats.c` | 路由、分配作用域、sink 公共操作 |
| `book_fs.c/h` | 文件/目录输入、存储锁与存储代次检查 |
| `book_stream.c` | ZIP 迭代解压、资源路径、CRC 和流生命周期 |
| `book_xml.c` | 带命名空间的 SAX2/XML 与 HTML push 解析 |
| `book_markup.c` | 公共段落、标题、样式、表格文本、图片及链接 |
| `book_markdown.c` | 分块按行解析 Markdown |
| `book_epub.c` | container、OPF、nav/NCX 和分卷资源 |
| `SConscript`、`sources.py` | 局部 Object 构建和完整源文件清单 |

父 SConscript 可带独立 `variant_dir` 加载本目录 SConscript。编译环境通过 Clone 隔离，仅第三方源码启用 `-Wno-error`。目标定义 `BOOK_FORMAT_TARGET`，使用 DFS `open/read/lseek/close`；每次操作持有 `storage_lock()`，检查路径可用性和打开时的 `storage_revision()`，锁在调用 sink 前释放。主应用的 `cancelled` 仍负责会话取消、退出和存储切换。

miniz 禁用写 ZIP、stdio、zlib API/兼容名称和默认堆分配。libxml2 禁用网络、输出、DTD 验证、Schema、XPath、XInclude、动态模块和默认文件输入；所有 XML 分配绑定 `book_allocator_t`，默认诊断与环境读取被隔离。两个库都不依赖 zlib，也不修改 `Z_PREFIX`。目标保留 `snprintf/vsnprintf`、自然排序使用的 `qsort`、常规字符串、DFS 和存储接口依赖。

## 实际转换边界

正文和压缩资源采用有界流式输入，但转换入口仍顺序遍历整书。现有文档 sink 会在转换期间生成全部插图缓存，并非等到 UI 首次显示对应页时才生成。转换器自身不常驻整本正文或全部图片，但整书缓存占用由主应用控制。

PNG/JPEG 读取、解码、CRC、取消和内存不足错误均向上传播，不会静默去掉图片后继续生成纯文本。引用 SVG/GIF 或内联 SVG，返回不支持。图片像素检查和缩放由主应用的共享图片 sink 完成。

- EPUB 按 OPF spine 顺序读取，nav/NCX 保留嵌套目录。分卷优先采用有效 spine 映射，未列入 spine 的章节包按自然序追加；不解析 `meta.json` 的私有顺序约定。资源依次查当前章节包、基础包。
- HTML/XHTML 支持段落、标题、列表、表格单元文本、图片和链接。CSS 仅支持标签、class、ID、`tag.class` 选择器，以及粗体、斜体、对齐和预格式化；不实现完整级联、复杂选择器、固定版式、字体加载或数学排版。HTML 使用容错 SAX 解析。
- Markdown 支持 ATX 标题、列表文本、引用、围栏代码、行内强调/代码/链接/图片和基础管道表格。表格保留分隔符。不实现完整 CommonMark/GFM、引用式链接、Setext 标题，以及跨 8 KiB 分块边界的复杂行内语法。输入为 UTF-8。

元信息/目录标题最多 511 字节，路径最多 1023 字节，XML 嵌套最多 96 层；manifest、spine 和分卷索引按实际申请结果增长，ZIP 文件数不设额外门槛。内联/外链样式表、简单规则列表和选择器按实际内容申请内存，不再按固定字节数或规则数截断；申请失败返回错误并清理资源。标题超限仍会截断，其余固定缓冲结构超限返回错误。

转换工作内存按需向应用分配器申请，不设置单次分配和累计占用的固定门槛；申请失败返回 `BOOK_NO_MEMORY`，转换作用域退出时释放已申请资源。分配长度仍检查整数溢出。ZIP 条目以流式方式展开，不额外限制解压后大小；压缩输入受目标 32 位 seek 范围限制，输出受实际存储容量及文档缓存寻址范围限制。DRM 和密码保护内容不解密。

## 独立检查

POSIX 主机执行 `python3 tests/build_host.py`，然后执行 `python3 tests/test_formats.py`。设置 `SANITIZE=1` 构建启用 AddressSanitizer/UndefinedBehaviorSanitizer。生成的样本和对象仅位于忽略的 `tests/build/`。

Windows 执行 `tests/check_arm.ps1`，使用本地 ARM 工具链和实际项目头文件编译转换器与库，检查非法 libc 堆/FILE/默认日志导入并生成 `.su` 栈报告。该脚本不构建主固件。

XML 的 4 KiB 输入缓冲和约 3.6 KiB 路径工作区由传入分配器提供。32 KiB 工作线程还需容纳主应用图片解码 sink 的栈；静态栈报告不能代替目标线程高水位测量。

最终回归覆盖、验证边界、逐函数栈帧与调用链小计见 [VALIDATION.md](VALIDATION.md)。
