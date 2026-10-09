# 转换器回归与栈用量

## 动态 CSS 容量回归

2026-10-02 使用普通主机构建及 AddressSanitizer/UndefinedBehaviorSanitizer 重新运行 `tests/test_formats.py`。超过 128 条规则、超过 64 KiB 外链样式表、超过 16 KiB 内联样式表和超过 95 字节的选择器均能应用尾部样式。该样本逐一注入分配失败，返回成功或 `BOOK_NO_MEMORY`，作用域退出后的未释放计数均为零。其余格式功能、取消、重复转换和错误传播检查通过。

完整测试未通过：连续正文大样本的分配器峰值为 6,365,690 字节，超过既有 1 MiB 断言。正文完整性检查通过，内存检查失败。当前 `HTMLparser.c` 的 `XML_PARSER_CONTENT` 分支等待后续 `<` 才调用 `htmlParseCharData()`，因此分块输入不能保证连续文本的解析缓冲恒定。测试将峰值断言放在其他检查之后执行，仍保留失败退出码；未调整预算或修改第三方解析器。

## 既有回归记录

以下是格式精简前、与当前保留格式相关的验证记录，不代表本次精简后的重新运行结果。既有主机回归通过。构建使用 GCC、`-O1`、AddressSanitizer 和 UndefinedBehaviorSanitizer；运行设置 `detect_leaks=1:halt_on_error=1` 与 `UBSAN_OPTIONS=halt_on_error=1`。测试同时检查进程退出码、标准错误输出和自定义分配器的未释放计数。

- EPUB 3：OPF spine 顺序、带命名空间的元信息、嵌套 nav、相对链接、百分号编码资源路径、外链 CSS 和图片流。
- EPUB 2：NCX 嵌套目录和未压缩 ZIP 条目。
- 分卷 EPUB：chapter1/2/10 自然顺序、章节 ID 和基础包图片回退。
- Markdown：中文长行、强调、链接、图片、围栏代码和基础管道表格。
- 错误路径：损坏 XML、外部实体声明、图片回调错误、不支持图片、仅消费一个字节后的 ZIP CRC 校验、取消和分配失败。

最后修复了 HTML SAX 回调失败时的终止方式：保留当前输入缓冲，禁止后续回调，当前块返回后退出。解析器创建期间发生内存不足时，错误回调还可能早于解析器指针赋值，错误处理已增加判空。这两条路径均进入最终检查。

MOBI `sample-ncx.mobi` 的共享 markup 联测另外修复了两个指定分配失败点。第 508 次分配失败留下空属性值，适配层已避免向零长度 `memcpy` 传入空指针。第 548 次失败使旧 HTML 解析器进入 EOF，但属性恢复循环继续调用不会前进的 `xmlNextChar()`；`HTMLparser.c` 现检测终止状态、清理已解析属性并退出。两个单点在 ASan/UBSan 下均返回 `BOOK_NO_MEMORY`（-3），测试程序退出码为零，且通过未释放分配、DFS 句柄及存储锁状态断言。正常转换基线仍为 706 次分配、117909 字节分配峰值、221 字节输出文本。

移除定位用的超时回溯钩子后，MOBI 现有常规回归也通过：80 个取消点、80 个模拟存储变化点、回调失败和重复打开均无残留分配或 DFS 句柄。联测使用 `codecs/mobi/tests/build_common.py` 的同一源码及 ASan/UBSan 配置，额外使用 `-no-pie -rdynamic` 定位调用地址，将产物重定向到 `formats/tests/build/mobi/`；没有修改 MOBI 源文件或其已有测试二进制。两个单点分别以 `regression <sample-ncx.mobi完整路径> 0 508` 和 `0 548` 执行，常规回归使用 `regression <sample-ncx.mobi完整路径> 0`。

图片测试 sink 验证流字节和错误传播，不执行真实 PNG/JPEG 像素解码；图片样本不是有效的完整 PNG。像素解码、灰阶缩放、真实 TF 热插拔以及目标线程栈高水位不属于本次主机回归已证明的范围。

## 编译与格式检查

独立 ARM 检查使用 GCC 14.2.1、Cortex-M33、Thumb、`-Os -fPIC -fstack-usage` 和项目实际头文件。自写源码使用 `-Wall -Wextra -Werror`；第三方源码独立编译。全量独立编译及禁止导入检查通过。最后的错误路径修改已包含在 `dpi-hdk_lb57gyd7n6_epd_hcpu` 完整固件构建中，构建成功，书架及 MOBI 私有解码库的导入符号全部匹配固件。

禁止导入检查没有发现 `malloc/free/realloc`、newlib `_impure_ptr`、`fopen/fread/fseek/fclose`、默认文件诊断和环境读取等依赖。允许依赖保留 `snprintf/vsnprintf`、`qsort`、字符串操作、DFS 与 storage 接口。最终书架包包含本轮错误路径修复，文件哈希和固件构建标识已通过独立核验。

自写 `.c/.h` 和测试 C 文件的 clang-format 19.1.5 检查通过。共享 `book_codec.h` 与 vendor 文件不参与此次格式化。

## ARM 静态栈数据

以下数值来自 `tests/build/arm/*.su`，表示编译器报告的单个函数栈帧，不是线程运行峰值。数据来自最后一次独立 ARM 检查；之后的 HTML 停止处理修改没有新增大数组，但最终目标机器码的栈数据仍应以增量构建为准。

| 函数 | 栈帧，字节 | 说明 |
| --- | ---: | --- |
| `book_format_convert` | 120 | 公共入口 |
| `bf_epub` | 3528 | 包含被内联的目录扫描工作 |
| `render_part` | 1096 | 章节资源路径 |
| `book_markup_convert` | 32 | markup 状态由分配器提供 |
| `bf_xml` | 208 | 4096 字节输入缓冲已移至分配器 |
| `htmlParseChunk` / `htmlParseStartTag` | 56 / 72 | HTML push 路径 |
| `start_html` | 48 | SAX 适配回调 |
| `markup_event` | 3664 | 标签属性和资源 ID 工作区 |
| `bf_picture` / `mapped_image` | 1088 / 1040 | 图片 ID 与分卷 ID 映射 |
| `bf_package_open` / `bf_zip_entry` | 1056 / 1136 | 资源打开；返回后才调用图片 sink |
| `bf_resolve` | 64 | 约 3.6 KiB 路径工作区已移至分配器 |
| `bf_markdown` / `md_open` | 3696 / 2072 | Markdown 与文件资源定位 |
| `mz_zip_reader_read_central_dir` | 4248 | 打开 ZIP 时使用，与图片解码不同时驻栈 |
| `mz_zip_reader_extract_iter_read` / `tinfl_decompress` | 64 / 240 | 实际使用的流式解压路径 |

EPUB 图片回调入口的可见调用链为：

```text
book_format_convert → bf_epub → render_part → book_markup_convert → bf_xml
→ htmlParseChunk → htmlParseStartTag → start_html → markup_event
→ bf_picture → mapped_image → 主应用 image sink
```

上述进入 sink 前的已列栈帧合计 10952 字节，约 10.70 KiB。打开图片条目的另一分支包含 `bf_package_open`、`bf_zip_entry` 和迭代器创建，已列栈帧小计约 12 KiB。资源打开函数返回后才进入 sink，两个分支不能直接累加。

这些小计支持转换器可见路径小于 32 KiB 的判断，但不是完整调用图的严格上界。主应用图片解码、分配器、DFS、libc 内部调用及线程入口还会占栈，32 KiB 文档线程的最终余量必须由板端栈高水位测量确认。

miniz 的整条目解压辅助函数在 `.su` 中可见约 11–12 KiB 单帧，但转换器不调用这些函数；转换器使用迭代器和分配器持有解压状态。自写正文遍历没有递归大栈。

## 交接路径

- `formats.h`、`book_internal.h`、`book_fs.h`。
- `formats.c`、`book_fs.c`、`book_stream.c`、`book_xml.c`、`book_markup.c`、`book_markdown.c`、`book_epub.c`。
- `SConscript`、`sources.py`、`.gitignore`、`README.md`、`VALIDATION.md` 和 `tests/` 源文件。测试产物位于忽略的 `tests/build/`。
- `../third_party/miniz/`：项目 miniz 副本、配置和 ZIP64 分配回调修补，具体见其 `BOOKS_PATCHES.md`。
- `../third_party/libxml2/`：选取的 2.6.24 源码、头文件、裁剪配置及分配/IO 隔离，具体见其 `BOOKS_PATCHES.md`。原许可保留。

公共 ABI、串行分配作用域、资源所有权和实际格式边界见 `README.md`。
