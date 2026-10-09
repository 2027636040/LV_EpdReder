# 单词应用

## 功能与内容组织

`words.so` 提供离线查词、今日学习、生词本、学习范围和完整词条详情。ECDICT 和 kajweb/dict 在电脑端合成为一个词库；查词和学习使用相同词条，用户不需要选择来源或词书文件。

查询顺序是完整词头、词形映射、前缀候选。一次展示最多八个候选，超过时提示补充字母。查询键仅折叠 ASCII 大小写；学习身份保留原始大小写，`Apple` 和 `apple` 不会自动合并。屈折词形只用于查找对应词条，不改变学习记录身份。

详情分别显示音标、释义、词形、例句、短语、同根词、范围和来源。整合规则为：

- 相同原始词头关联为一个词条；同一种内容经空白规范化后完全相同才合并，并保留全部来源。
- 不同音标、释义和译文并存，不自动覆盖。音标保留英式、美式和未分类标记。
- ECDICT 的中英文释义没有可靠的一一对应关系，因此分别保留；kajweb 同一 trans 对象内的中英文可以共同显示。
- 例句与短语保留在词条层级，不推断它们属于哪条释义。
- 同根词不当作屈折词形别名，避免把不同词误查成同一词。

首页与设置显示“四级、六级、雅思”等学习范围，不显示 JSON 或 WDB 文件名。范围由预生成的成员索引表达；同词属于多个范围时不重复存储正文，不重复安排新词。范围只控制后续新词，查词仍检索整个已部署词库。提供哪些范围由部署资源决定，不要求多范围组合或词频排序。

## 学习算法、身份与迁移

继续使用 FSRS-6：以整个单词为一张卡，查看答案后按四档评分。学习顺序为到期学习/重学、到期复习、随后新词。默认每天 20 个新词，可在设置中调整为 0～200；额度不限制到期复习。每完成三个范围内新词，优先补入一个未学习的收藏词；范围耗尽后由生词本补充，两者共用额度。

永久身份为 `en:<NFC 原始词头>`，不是词条序号或文件偏移。学习记录保存原始词头，词库重新生成后重新按词头关联。切换范围只重置新词游标，保留已学进度、旧范围到期复习、未评分卡片、每日计数、收藏和暂停状态。移出生词本不删除学习记录。

| 数据 | 升级处理 |
| --- | --- |
| WDB1 词库 | 继续可读，以“全部词汇”表示其成员范围；不要求设备转换原始数据 |
| 旧六字段内容快照 | 继续可读；词库有相同词头时按需补充为新内容，保留 FSRS 状态 |
| 旧学习记录块 | 二进制结构不变，按原始词头共用记录 |
| 旧配置 version 1 | 保留额度和未评分会话；迁移为范围配置，重新建立新词游标 |
| 新词库 generation 变化 | 清除旧索引关联和范围游标；按词头恢复学习内容，不删除旧进度 |

应用清单 version 为 3，data_version 仍为 1，允许平台进行保留数据的原位升级；配置自身的版本由应用迁移。同一安装卷内使用“更新应用”；从内部 Flash 转移到 TF 卡时，按下文的跨卷安装步骤操作，不手动删除学习数据。

调度核心 `core/words_fsrs.c` 不依赖 RTOS、文件系统或 LVGL。使用 FSRS-6 默认 21 参数、90% 目标回忆率、60/600 秒学习步骤、600 秒重学步骤、最长 36,500 天，关闭随机微调。RTC 未设置时仍能查词，但不能评分；本地 RTC 在调度入口转换为 UTC。

## 存储、按需读取与生命周期

数据随安装卷保存：内部安装为 `/flash/data/words`，TF 卡安装为 `/sdcard/.epd/data/words`。

| 文件 | 用途 |
| --- | --- |
| config.dat | 范围 ID、词库 generation、新词游标、额度和未评分会话 |
| states/000.dat 等 | 每块 32 条学习记录，复用平台带 CRC 的记录接口 |
| content.bin | 学习与收藏词的内容快照，按长度和偏移读取 |
| library.wdb（可选） | 管理员部署的统一词库替换文件 |

统一词库按以下优先级选择，查词与学习共用该规则：

1. 应用数据目录的 `library.wdb`。
2. `/sdcard/words/library.wdb`。
3. 应用安装目录的 `res/library.wdb`。

扩展资源是同一逻辑词库的完整生成产物，不是第二个供用户切换的数据源，也不与随包库按文件分别排学习队列。旧版 `book.wdb` 和 `books/` 文件不再作为词书入口；不会主动删除这些旧文件，已学习内容仍保存在独立快照中。

格式说明见 [WDB2.md](WDB2.md)。设备按预生成索引读取单条内容，不加载完整词库，不解析 CSV/JSON，不在首次运行时扫描建索引。词条和展示缓冲按实际长度申请；不设 32 KiB 等固定正文上限。检查整数运算、文件实际大小、索引和记录边界、读取及分配结果。学习记录索引按需增长，申请失败保留已有记录并报告错误。

受管后台线程是业务数据唯一写入者，不操作 LVGL 控件。页面通过请求代次领取独立结果快照；页面退到后台停止自己的查询定时器，页面销毁释放结果和详情文本。只读任务可取消，已提交的评分和收藏等写操作按顺序完成。评分先更新内存，再后台保存；正常退出同步待保存数据，失败可重试。

文件句柄不跨任务保留。结果携带词库 generation，进入详情和使用成员序号前重新核对，禁止旧索引读取新文件。更新资源应在应用退出后，用完整生成文件替换，不原地边写边读；再次进入后重新读取索引。generation 检查不是每次开文件计算整库 SHA256，不能代替部署过程的完整性校验。

TF 卡移除沿用平台的取消、停止服务和退出流程；模块检查安装卷会话及外部文件可用性，释放句柄和快照，不继续访问旧控件。重新插卡并确认加载后重新打开应用。突然拔卡或断电可以丢失尚未保存的操作，不会把另一词条的序号当成原词身份。

## 资源来源与授权

- [ECDICT](https://github.com/skywind3000/ECDICT)：快照 `bc015ed2e24a7abef49fc6dbbb7fe32c1dadaf8b`，MIT，许可见 LICENSE.ecdict。
- [kajweb/dict](https://github.com/kajweb/dict)：快照 `3992bcb94c800a2fd38a9fd6ff95b2353e755363`。该快照没有明确开放许可证，仓库说明数据整理自有道背单词。包含其内容的产物用于本地开发验证；对外再分发前需要取得权利方许可，不能套用 ECDICT 的 MIT 许可。未打包音频和图片。
- Py-FSRS 对照版本为 `9446cb06605c597a063aeee49f7d188d42e34dc2`（6.3.2），许可见 LICENSE.fsrs。

实际输入文件、catalog 分类、版本、摘要、记录数量、来源关联、拒绝原因和去重统计见随库 `.wdb.json` 报告；报告不再把所有内容标为 ECDICT。来源概要 SOURCES.txt 随安装包分发。

## 电脑端生成

生成器只依赖 Python 标准库。在本模块目录运行以下命令。输入由开发者提前从上述固定快照取得，正常构建不会下载或转换资源。

ECDICT 输入为 ecdict.csv，固定快照的 SHA256 为 `1a6947e04785db63613a92e14903cdae7954f7e84860b10e68e5c7cbb3f9c3cf`。kajweb 输入使用真实 JSON/JSON-lines，或仓库中包含 JSON 的 ZIP；同时提供同快照 bookLists.txt 分类目录。

已核查的 kajweb 文件如下（本地可以按左列重命名）：

| 本地文件 | 仓库路径 |
| --- | --- |
| CET4luan_1.zip | book/1523620217431_CET4luan_1.zip |
| CET4_3.zip | book/1521164643060_CET4_3.zip |
| CET6_3.zip | book/1521164633851_CET6_3.zip |
| KaoYan_3.zip | book/1521164658897_KaoYan_3.zip |
| IELTS_3.zip | book/1521164666922_IELTS_3.zip |
| TOEFL_3.zip | book/1521164667985_TOEFL_3.zip |

默认安装包携带完整统一词库：保留全部 ECDICT 有效词头，合并上述六份 kajweb 输入。四级的两份文件合并为一个范围。

```powershell
python tools/build_dictionary.py output/source/ecdict.csv output/dictionaries/unified-full/library.wdb --kajweb output/source/kajweb/CET4luan_1.zip output/source/kajweb/CET4_3.zip output/source/kajweb/CET6_3.zip output/source/kajweb/KaoYan_3.zip output/source/kajweb/IELTS_3.zip output/source/kajweb/TOEFL_3.zip --catalog output/source/kajweb/bookLists.txt
```

工具不覆盖已有输出，更新时指定新生成目录。每次同时产生 `library.wdb` 和 `library.wdb.json`。实际使用其他来源快照时，传入 `--revision` 和 `--kajweb-revision`，不要沿用默认版本号。

`tools/scope_map.json` 明确映射 ECDICT tag 与 kajweb catalog tag，例如 ECDICT 的 ky 对应考研，kajweb 的 IELTS 对应雅思。映射根据实际目录建立，不靠文件名猜测；未知标签和异常条目进入报告。不同来源的范围成员取并集，不宣称各来源覆盖面或版本完全一致。

`--include-scopes cet4,cet6` 是电脑端资源裁剪选项：保留这些范围的词条并集和这些范围的索引，不生成多个重复正文库；这不是设备上的多范围组合功能。不传该选项则保留全部有效词头，未属于范围的词仍可离线查词。`--legacy` 只用于生成 WDB1 迁移测试数据。

本次数据规模约为：

| 产物 | 词条 | WDB 容量 | 部署 |
| --- | ---: | ---: | --- |
| 完整统一词库 | 770,633 | 213,991,764 字节（204.08 MiB） | 直接随应用安装包，安装到 TF 卡 |

随包词库提供四级、六级、雅思、托福、考研及 ECDICT 中的 GRE、高考、中考范围，安装后即可使用，不需要另行复制扩展库。数量以本次输入快照和生成报告为准，不是考试官方词汇总数。

## 构建、打包与安装

平台与动态应用独立构建。已有兼容固件时只需更新单词应用。本次未新增宿主导出，不需要为词库改造修改 SDK、屏幕驱动或分区。

只有显式构建单词应用时才需要 Node.js、sharp 和预生成词库。入口图标使用 `assets/icon.svg`；在本模块目录执行 `npm install --no-save --package-lock=false sharp`，或让 NODE_PATH 指向已有安装。

在仓库根目录执行：

```powershell
. .\SiFli-SDK\export.ps1
scons -C project --board=dpi-hdk_lb57gyd7n6_epd_hcpu -j8
scons -C modules APPS=words -j8
```

默认读取 `modules/words/output/dictionaries/unified-full/library.wdb` 及旁边的报告，直接打入安装包。不在每次编译时重新生成。使用另一个预生成产物：

```powershell
scons -C modules APPS=words WORDS_DICTIONARY=<词库绝对路径.wdb> -j8
```

同时构建平台和单词包可使用 `scons -C project --board=dpi-hdk_lb57gyd7n6_epd_hcpu APPS=words -j8`。批量应用构建和工厂预装见 [平台与应用构建](../../docs/BUILDING.md)。

标准安装包自动更新到：

```text
project/build_dpi-hdk_lb57gyd7n6_epd_hcpu/app-resources/words/
    app.json
    files.sha256
    words.so
    icon.ezip
    res/library.wdb
    res/library.wdb.json
    res/LICENSE.ecdict
    res/LICENSE.fsrs
    res/SOURCES.txt
```

仅重新打包已有 words.so 时，在本模块目录执行 `python tools/package_words.py`，使用相同默认词库与标准输出目录。可用 `--firmware`、`--dictionary` 指定输入；`--output` 另存新归档目录。该命令不会编译模块。

把整个包复制到 TF 卡 `apps/words`。内部文件系统总量约 5.48 MiB，不能容纳 204.08 MiB 的随包词库，安装步骤根据现有安装位置区分：

- 尚未安装：在系统应用安装中选择单词应用，安装位置选 TF 卡。
- 已安装在 TF 卡：直接“更新应用”，保留已有数据。
- 已安装在内部 Flash：先退出单词应用，通过系统应用管理卸载，再将新包安装到 TF 卡。平台卸载只删除应用文件和缓存，保留 `/flash/data/words`；如果 TF 卡尚无该应用的数据目录，平台安装后会复制并校验旧数据，再清理已迁移的源数据。不要手动删除 `/flash/data/words`。

平台的原位更新不会切换安装卷。如果 TF 卡已经存在 `.epd/data/words`，平台保留 TF 卡中的记录，不自动合并内部 Flash 的另一份记录；需要迁移内部记录时，应在安装前备份并移走 TF 卡上已有的该目录。

TF 安装目录 `.epd/apps/words` 与安装包来源目录分开，数据与缓存也随卡保存；首次安装时需要同时容纳来源包与安装后的文件。不要把动态应用安装包交给固件串口下载脚本。

默认使用安装包中的 `res/library.wdb`。以前单独部署在应用数据目录或 `/sdcard/words/library.wdb` 的文件仍有更高优先级；部署人员需要同步更新或移走旧替换文件，才能使用新版随包库。用户界面只有查词和学习范围，不增加文件选择入口。

## 测试

主机测试使用生产 C 读取器、FSRS、服务和持久化代码；文件系统、内存申请和 RTOS 调度入口由测试替身提供。主机测试不能验证真实抢占、物理拔卡和 LVGL 显示。

在 Visual Studio 开发命令行中进入本模块目录，先创建 output 目录，构建测试库：

```text
cl /nologo /std:c11 /utf-8 /O2 /MT /LD /W4 /D_CRT_SECURE_NO_WARNINGS core\words_fsrs.c core\words_dictionary.c core\words_content.c /Fo:output\ /Fe:output\words_core.dll /link /EXPORT:words_fsrs_review /EXPORT:words_dictionary_open /EXPORT:words_dictionary_search /EXPORT:words_dictionary_entry /EXPORT:words_dictionary_entry_size /EXPORT:words_dictionary_read_entry /EXPORT:words_entry_parse /EXPORT:words_entry_text /EXPORT:words_dictionary_scope /EXPORT:words_dictionary_member /EXPORT:words_dictionary_find /EXPORT:words_dictionary_word /EXPORT:words_dictionary_unchanged
cl /nologo /std:c11 /utf-8 /O2 /MT /LD /W4 /D_CRT_SECURE_NO_WARNINGS /Itests\shim /Icore tests\service_harness.c words_result.c words_store.c core\words_learning.c core\words_fsrs.c core\words_dictionary.c core\words_content.c /Fo:output\ /Fe:output\words_learning.dll
python tests/test_core.py --library output/words_core.dll --reference <上述固定版本Py-FSRS目录>
python tests/test_unified.py --library output/words_core.dll --dictionary output/dictionaries/unified-full/library.wdb
python tests/test_learning.py --library output/words_learning.dll
```

覆盖内容包括：跨来源补充与去重、音标/释义/例句/短语、范围去重、大小写身份、索引重排后进度、配置与旧内容迁移、未评分恢复、跨范围到期复习、损坏文件、读取/分配/保存失败、取消任务、卡不可用与重新加载、退出资源释放。FSRS 对照固定 Py-FSRS 的 128×128 次状态转换，共 16,384 次。全量 WDB 的 770,633 个词条经过 C 读取与格式化检查。

上板验证：

1. 分别验证 TF 卡原位升级和内部 Flash 转移到 TF 卡，确认额度、未评分词、收藏、暂停和到期复习保留。
2. 查找同词并进入学习及完整详情，检查音标、例句和短语；切换四级与六级，检查重叠词不重新算新词。
3. 退出应用后替换扩展词库，重新进入，检查相同单词仍关联原进度。
4. 查词、进入详情和评分过程中快速返回；TF 卡移除后重新插入加载，检查无旧回调、无文件句柄遗留。
5. 长时间停留不操作、反复进出详情，检查没有轮询重绘，退出后动态内存回收。

模块链接沿用固件浮点 ABI，数学库的 wchar_t 4 字节与 SDK 2 字节警告仍保留；模块与数学库不传递 wchar_t。主机测试通过和编译成功不等同于完成上板验证。
