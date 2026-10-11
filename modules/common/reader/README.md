# 共享阅读组件

每个动态应用分别编入此目录的 `SConscript`，使用同一份字体、结构化文档、分页和显示源码。组件没有线程入口要求，也不需要安装 `books.so`。脚本已包含 `document.c`、`document_cache.c`、`document_image.c`、`book_image.c`、格式转换器、XML/miniz 和公共 image 源码，调用方不再重复添加这些源文件。

## 章节输入

`epd_reader_open()` 接受已完整缓存的 UTF-8 TXT 或 XHTML 文件。`reader_options_t` 指定应用 ID、派生缓存目录、标题、稳定内容身份、恢复位置和回调。内容身份应包含账号、书籍、章节及内容版本；如果图片版本发生变化，也应更新内容身份。缓存键包含应用 ID 和完整内容身份，不使用章节数组下标。

路径必须少于 256 字节，应用 ID 必须为 1～63 个 ASCII 字母、数字、点、下划线或连字符，`strlen(app_id) + strlen(content_identity)` 必须小于 254。缓存目录必须为绝对目录且长度加 32 小于 `STORAGE_PATH_MAX`。超长输入会返回失败，不会截断身份或路径。标题用于显示，可以截短到内部标题缓冲区。

`resources.open` 在 worker 中收到原始资源引用和章节文件路径。回调可以下载并缓存 HTTP 图片或样式，然后返回完整的 `book_stream_t`；转换器负责消费并关闭返回的流。打开返回 `BOOK_IO_ERROR`、`BOOK_ERROR` 或 `BOOK_UNSUPPORTED` 时，共享适配器标记资源缺失：图片显示占位，缺失的 CSS 被跳过，正文按默认排版及其余可用样式继续显示。本次派生文档不持久化，以便下次打开重试。取消、内存失败和资源打开成功后的读取错误继续向上传递；books 原有资源回调的普通错误不作缺失回退。

## 线程与生命周期

UI 调用 `epd_reader_open()` 排队。应用现有 service 循环串行调用 `epd_reader_worker_step(service)`；一次调用可能完成整章转换，返回 `true` 表示处理了工作。工作循环需要自行轮询或在 UI 排队后唤醒。转换、资源回调和图片像素准备都在 worker 中运行，不调用 LVGL。

屏幕空闲时，UI 调用 `epd_reader_step(budget_ms, foreground, commit)` 和 `epd_reader_bind(body)`。分页、字形度量和控件绑定由 UI 执行。`commit=false` 保持可见页不变；只有前台提交才更新阅读进度。`epd_reader_get()` 返回的 `page` 是可见页码，`pages` 在分页完成前为零，完成后为当前章节总页数；`indexing` 表示分页尚未完成。重排期间 `page/ready` 仍描述旧布局的可见页，调用方应等待恢复页完成绑定，再根据新页码执行翻页。

UI 可调用 `epd_reader_seek()`、`epd_reader_seek_cursor()`、`epd_reader_reflow()` 和 `epd_reader_cancel_pending()`。恢复位置以结构化记录偏移和记录内字节偏移表示；只有内容身份和 `DOCUMENT_CONVERSION_VERSION` 匹配时才能复用。`save` 在 UI 线程调用，必须在返回前复制位置；需要后台写盘时，由应用排队保存。

`epd_reader_open()` 接收非空 options 后取得 `user` 的释放责任。失败时立即或在后续收集时执行 `release_user`；成功后直到 UI 关闭且 worker 归还会话才执行。`release_user` 始终在 UI 线程执行一次；它可以释放该会话拥有的 `resources.user`。`cancelled` 可能在 UI 或 worker 中检查，必须只做非阻塞、线程安全的状态读取。

普通切章使用 UI `epd_reader_close()` 和定期 `epd_reader_collect()`，worker 继续处理取消和清理。完整停止顺序为：禁止新请求，取消 service/会话，让最后一次 worker step 返回，在同一个 worker 调用 `epd_reader_worker_shutdown()`，等待 worker 退出，最后在 UI 调用 `epd_reader_close()` 和 `epd_reader_collect()`。组件不会强杀线程，也不会在后台删除控件或字体。

## 主机回归

在项目根目录运行：

```sh
python3 modules/common/reader/tests/build_manifest_test.py
python3 modules/common/reader/tests/run.py --sanitize
python3 tests/run_reader_view_tests.py --sanitize
python3 tests/run_service_tests.py --sanitize
python3 modules/books/document/tests/run_cache_service.py --sanitize
python3 modules/books/document/tests/cache/run.py --sanitize
```

章节测试链接实际 TXT/XHTML 转换、文件流、XML/miniz、document、缓存和存储实现，以主机替身模拟 UI 边界和图片解码；视图测试执行实际公共分页及绑定代码。ASan/UBSan 检查主机内存和未定义行为。固件链接、真实字体/图片解码和屏幕时序仍需在目标平台验证。
