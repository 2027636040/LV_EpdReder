# 微信读书客户端

`wr_client.h` 提供同步、串行调用的协议接口。一个 client 的请求、getters、导入导出和销毁均由同一个 worker 串行执行。网络操作调用创建时传入的取消回调；取消后丢弃该次操作的会话、Cookie 和输出变化。客户端自身不创建线程或访问文件。

所有拥有的字符串使用 `epd_app_alloc(..., EPD_APP_PSRAM)` 分配，由调用者使用 `epd_app_free()` 释放。书目和章节数组使用对应的 `wr_books_free()`、`wr_chapters_free()` 释放；两个账号 getter 返回借用字符串。getters 接收 NULL 时返回空字符串，`authenticated(NULL)` 返回 false。

## 编入模块

生产源文件：

```text
client/wr_client.c
client/wr_json.c
client/wr_models.c
client/wr_protocol.c
client/private/wr_cjson.c
client/private/wr_md5.c
client/private/wr_base64.c
```

头文件搜索路径需要 client、`modules/common/http` 和提供 `epd_memory.h` 的平台目录。外部依赖为 `epd_http_perform/epd_http_response_clear`、`epd_app_alloc/epd_app_free` 和标准 C 运行库（包括 `time`、格式化及数字转换函数）。SConscript 使用 `LIBS=['gcc']`，按模块浮点 ABI 静态链接需要的编译器支持函数。私有 mbedTLS 配置仅启用 MD5/Base64；生产构建不需要定义自测宏。cJSON 符号使用 `wr_cjson_` 前缀，mbedTLS 符号使用 `wr_mbedtls_` 前缀，不能再重复编入这些算法的 SDK 同名实现。

Cookie 按 client 保存，支持逐条 Set-Cookie、Domain、Path、Expires、Max-Age 和删除。已确认的账号与返回 Cookie 中的账号不一致时，请求失败。公共 HTTP 负责完整响应、TLS 和取消；客户端不接收失败请求的部分内容。Date 用于章节参数的服务器时间，缺少 Date 时使用系统 UTC 时间。

登录、书架、目录和章节 API 均沿用参考 `weread_http.c` 的共用请求头：`User-Agent: WeRead/1.0.0 WRBrand/remarkable wr_eink`、`Accept: application/json, text/plain, */*`、`CLIENT_TYPE: 1` 和 `X-Is-Serverside: 1`，POST 使用 `Content-Type: application/json`。章节 Referer 使用编码后的书籍 URL，其他 API 使用网站根地址。Connection 和 Content-Length 由公共 HTTP 传输层生成。

## 登录、续期与缓存

`login_begin` 返回拥有的二维码 URL。`login_poll` 查询一次扫码结果；确认后在同一次调用中执行 weblogin 和 session/init。session/init 失败后保留待完成状态，下一次 poll 重试 init。只有完整流程成功才返回 WR_OK。WR_PENDING 表示继续等待扫码，WR_EXPIRED 表示二维码已过期。

session JSON 的版本为 1，包含账号、令牌、Cookie 和登录完成状态。导入操作整体成功后才替换原会话；导入已登录状态后即可通过 getter 读取离线账号，authenticated 为 true。导入不访问服务器。shelf/catalog/chapter 遇到明确的认证失效时续期一次，然后重新执行整个 fetch 一次；再次失败直接返回错误。章节重试从 e_0 重新开始。

`wr_books_json` 输出包含 id/title/author/format/cover/version/update_time 的 JSON 数组；`wr_chapters_json` 输出包含 uid/title/index/update_time 的数组。index、version、update_time 按十进制字符串输出，保留 int64_t 全部精度。解析接受服务器 updateTime 和缓存 update_time；书籍 version 缺失或为零时沿用参考实现读取 bookVersion。旧缓存缺少修订字段时置零，表示修订信息未知。两个 parse 接口同时支持这些缓存数组与服务器的书架/目录封装。书目和章节数量按输入动态分配，没有固定数量门槛。JSON 数字形式的标识和修订字段仅在可精确表示的整数范围内接受；更大的整数应使用 JSON 字符串，否则显式报错。

JSON 解析使用 cJSON 的长度接口并检查尾部完整消费。cJSON 内部解析失败统一映射为 `WR_ERR_PROTOCOL`，其中也可能包含解析器分配失败；该返回码不单独证明输入损坏。客户端能够直接识别的其他分配失败返回 `WR_ERR_MEMORY`。两种错误均清理中间对象，不返回部分结果。

章节按 e_0、e_1、e_3 顺序请求，分别检查 MD5，拼接后还原 shuffle 并解码 Base64。返回原始 TXT/XHTML，保留标签与实体。任意请求失败、业务拒绝或片段校验失败都不返回部分内容。

## 主机测试

在 POSIX 环境中，从仓库根目录执行 `python3 modules/weread/client/tests/run.py`，使用 C 编译器及 ASan/UBSan。

测试使用无真实凭据的模拟 HTTP 响应，包含协议固定向量、登录/轮询/init 重试、会话往返、100 本书与 700 章、长字段、续期及单次重试、取消时会话不变、Cookie 更新/删除/路径、权限错误、坏片段、分配失败和内存释放检查。

在 Visual Studio x64 Native Tools Command Prompt 中执行：

```bat
cd /d D:\program\sf-candio\modules\weread\client\tests
if not exist build mkdir build
cd build
cl /nologo /std:c11 /W4 /WX /fsanitize=address /Zi /D_CRT_SECURE_NO_WARNINGS /DWR_MBEDTLS_SELF_TEST /I..\include /I..\.. /I..\..\..\..\common\http ..\test_client.c ..\..\wr_client.c ..\..\wr_json.c ..\..\wr_models.c ..\..\wr_protocol.c ..\..\private\wr_cjson.c ..\..\private\wr_md5.c ..\..\private\wr_base64.c /Fe:test_client.exe
test_client.exe
```

`python tests/vector_reference.py` 使用 Python 标准库独立计算固定向量，无网络和文件 I/O。固定向量同时写在 C 测试中，避免只用生产代码生成期望值。
