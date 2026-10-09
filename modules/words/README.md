# 单词应用

## 当前实现

`words.so` 是按需运行的动态应用，提供首页、今日学习、离线查词、生词本、词书选择、词条详情分页和应用设置。退出详情恢复原页面；返回应用列表前同步待保存数据，随后停止受管线程。无任务时线程阻塞，请求完成后暂停页面定时器；没有光标闪烁、滚动字幕或转场动画。

查询顺序为完整词头、词形映射、前缀候选。一次最多展示八个候选；超过时提示补充字母，不猜测有歧义的原形。索引按需读取，词条缓冲上限为 32 KiB，不把完整词库加载到 RAM。

FSRS-6 已接入“查看答案—四档评分—下一个词”。评分单位是整个单词。同一词在不同词书和生词本中共用一份记录；移出生词本不删除已学进度，暂停学习是独立操作。未评分退出后从该词正面恢复，应用内从完整释义返回保留答案展开状态。

学习队列优先安排到期的短期学习/重学，其次到期复习，再安排新词。每日新词默认 20，可在应用设置中即时调整为 0～200；达到额度不限制到期复习。当前词书是主要新词来源，每完成三个新词后优先补入一个尚未学习的收藏词；词书耗尽后由生词本补充，两者共用额度。当前词书内部按词头索引顺序取词，尚未接入词频排序。

首页显示当前词书、待复习数量、今日新词和今日复习数量。统计区分当天首次学习、当天不同已学词的复习和评分次数，不因同日重复评分重复计算新词。RTC 未设置时可以查词，但不接受学习评分；本 SDK 的本地 RTC 时间在调度入口转换为 UTC。

## 学习数据和词书

数据目录随应用安装位置解析：内部为 `/flash/data/words`，TF 卡为 `/sdcard/.epd/data/words`：

| 文件 | 用途 |
| --- | --- |
| `config.dat` | 当前词书、每日额度、词书游标、未评分会话 |
| `states/000.dat` 等 | 每块 32 条学习记录，复用平台带 CRC 的记录保存接口 |
| `content.bin` | 已学习、已收藏和当前待学词的内容快照，按偏移读取 |
| `book.wdb` | 最近一次导入的词书 |

工作线程是这些数据的唯一写入者。只读请求离开页面后可以取消，已提交的评分、收藏和设置命令按先后顺序执行；响应携带请求代次，评分另外校验当前卡片与会话号。页面不持有后台可变记录，后台不访问控件。评分更新内存并发布下一词结果后执行后台保存；块切换需要先保存当前脏块，不会重写全部学习记录。正常返回应用列表和平台受控关机入口等待保存，失败则提示并允许重试。突然断电可能丢失最后尚未写入的操作。

学习索引按需增长，优先预留到 128 条的整数倍；预留申请失败时尝试本次所需容量。学习/收藏记录不设固定条数上限，扩容失败保留已有索引并报告内存不足；索引编号及分配长度仍检查整数范围。正文缓冲每个最多 32 KiB，状态块缓存只有一块。应用安装卷空间不足会返回写入失败。应用停止后释放索引、块缓存、页面快照及详情缓冲。

将电脑端生成的独立 `.wdb` 词书放入 TF 卡 `/words/books/`，或应用数据目录的 `books/`。词书页面分页列出这些文件、随包词书和最近导入的词书。选择外部词书时检查应用安装卷的剩余空间，分块复制到应用数据目录、核对写入内容并验证词条后再切换；中途返回会取消未完成的导入。内部安装的应用在拔卡后仍可读取已导入词书；卡内安装则连同词书和学习记录一起随卡携带。切换词书不会删除旧词的到期复习。

目前内部只保存一个导入词书槽位，新导入替换该槽位；学习内容快照另存，因此旧词仍可复习。完整查询库应放在 `/sdcard/words/library.wdb`，不要作为整本学习词书导入内部存储。

## 词库来源与格式

- ECDICT：`bc015ed2e24a7abef49fc6dbbb7fe32c1dadaf8b`，原始 CSV SHA-256 为 `1a6947e04785db63613a92e14903cdae7954f7e84860b10e68e5c7cbb3f9c3cf`。
- Py-FSRS：`9446cb06605c597a063aeee49f7d188d42e34dc2`，项目版本 6.3.2。
- 许可分别保存在 `LICENSE.ecdict` 和 `LICENSE.fsrs`，打包时随应用携带。

电脑端转换命令：

```powershell
python tools/build_dictionary.py <ecdict.csv> output/dictionaries/library.wdb
python tools/build_dictionary.py <ecdict.csv> output/dictionaries/cet4.wdb --tag cet4
```

工具不覆盖已有生成文件；新版本使用新输出目录。随词库生成 `.wdb.json` 报告，包含来源、摘要、容量、标签计数和被排除的条目。四级标签对应所选 ECDICT 快照，并非对当前考试大纲完整性的承诺。

当前二进制格式使用小端整数：64 字节头、104 字节定长词头索引、100 字节词形索引、按偏移定位的 UTF-8 词条。词条包含词头、音标、中文释义、英文释义、词形变化和标签，六个字段以 NUL 分隔。查询键仅折叠 ASCII 大小写；原始词头保留大小写、标点和 NFC 表示，可供后续学习状态使用固定身份 `en:<原始词头>`，不能使用索引序号作为永久身份。

当前构建结果：

| 内容 | 词条数 | 字节数 | MiB |
| --- | ---: | ---: | ---: |
| 四级独立词库 | 3,849 | 2,165,473 | 2.07 |
| 完整查询库 | 770,609 | 143,842,129 | 137.18 |

完整库排除了两条超过词头长度限制的异常记录，详情见生成报告。当前格式优先直接随机读取，尚未做索引压缩；词频排序及独立词书成员清单尚未接入。

设备按以下顺序选择词库：

1. 应用数据目录的 `library.wdb`：用户导入的词库。
2. `/sdcard/words/library.wdb`：TF 卡扩展词库。
3. 应用安装目录中的 `res/library.wdb`：随包备用词库，通过平台解析内部存储或 TF 卡路径。

从候选进入详情时继续使用同一个文件和内容标识。词库被替换时要求重新查询，不用旧偏移读取新文件。读取失败时关闭句柄并返回错误，不在后台直接操作页面控件。

当前平台在 TF 拔出时会统一停止动态应用。本模块遵从该流程，学习记录和学习快照保存到应用安装卷。
安装在内部存储时可重新打开；安装在 TF 卡时，重新插卡并确认加载后恢复入口。随包词书与导入词书路径随安装位置解析，已保存进度随数据目录携带，拔卡时未保存操作可以丢失。

## 调度核心

`core/words_fsrs.c` 不依赖 RTOS、文件系统和 LVGL。输入为旧状态、评分和 UTC 秒，输出为新状态和到期时间。使用 FSRS-6 默认 21 个参数、90% 目标回忆率、60/600 秒学习步骤、600 秒重学步骤、最长 36,500 天，关闭随机微调。时间倒退和无效状态返回失败，不改变输出。

以固定随机序列对照 Py-FSRS：128 条序列、每条 128 次评分，合计 16,384 次转换。比较阶段、学习步骤、到期时间、最后评分时间、稳定性与难度，并检查遗忘次数。此验证针对默认参数，不代表完成了设备端学习队列或 UI 验证。

## 构建和安装

主固件需要包含首批查词版本增加的通用 LVGL 键盘、输入框和文字测量导出。第二版学习接入复用已有导出，没有增加宿主接口；已使用首批兼容固件时，只安装新版单词包即可。SDK 指针仍为 `a25ebcc`，不需要修改 SDK 源码或分区。

只有选择构建单词应用时才需要 Node.js、`sharp` 和词库；平台及其他应用不依赖这些内容。
在本模块目录执行 `npm install --no-save --package-lock=false sharp`，或通过 `NODE_PATH` 指向已有依赖。
首次准备随包四级词库时，在本模块目录执行：

```powershell
python tools/build_dictionary.py <ecdict.csv> output/dictionaries/cet4.wdb --tag cet4
```

词库及同目录的 `cet4.wdb.json` 准备完成后，在仓库根目录激活 SDK 环境并显式选择单词应用：

```powershell
. .\SiFli-SDK\export.ps1
scons -C project --board=dpi-hdk_lb57gyd7n6_epd_hcpu -j8
scons -C modules APPS=words -j8
```

已有配套固件时，只需执行应用构建命令。该命令编译 `words.so`、从 `assets/icon.svg` 生成入口图标，并将四级词库、来源报告和两份许可打包到：

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
```

源码、图标、清单、词库、报告或许可文件改变后，再次执行 `scons -C modules APPS=words -j8` 会更新这个目录，不需要手动复制安装包。已有词库文件不会在构建时重新从 CSV 转换或从网络下载。
需要使用其他随包词库时，在仓库根目录执行 `scons -C modules APPS=words WORDS_DICTIONARY=<词库绝对路径.wdb> -j8`，同时提供该词库旁边的 `.wdb.json`；以后构建该词库时继续传入相同参数。
一次构建平台和单词包时使用 `scons -C project --board=dpi-hdk_lb57gyd7n6_epd_hcpu APPS=words -j8`。
批量构建见 [平台与应用构建](../../docs/BUILDING.md)。

把 `app-resources/words` 整个目录复制到 TF 卡的 `apps/words`，通过系统“应用安装”选择安装。
首次安装选择内部存储或 TF 卡；TF 卡安装目录为 `.epd/apps/words`，与安装包来源目录分开。
随包词库保留在所选安装目录，学习记录、配置及导入词书使用同卷的 `data/words`。
安装包不是固件镜像，不能直接交给串口刷写脚本。单词应用默认不加入出厂 `fs_root.bin`，安装它不需要重刷或格式化内部文件系统。

仅重新打包已有编译产物时，可以在本模块目录执行 `python tools/package_words.py`，默认也更新当前板级目录的 `app-resources/words`；该命令不编译模块。`--firmware` 可指定其他已构建的固件目录，`--dictionary` 可指定词库，`--output` 仅用于另存到尚不存在的归档目录。

仅测试四级查词时使用随包词库即可。需要完整库时，把生成的完整 `library.wdb` 放到 TF 卡 `words` 目录，并保留对应来源报告和许可。

模块链接时沿用编译器的浮点 ABI 选择 `libm`，并把需要的双精度辅助函数从 `libgcc` 链入模块。工具链会提示这些库的 `wchar_t` 为 4 字节，而 SDK 模块使用 2 字节；本模块与数学库之间仅传递数值，不传递 `wchar_t`。没有屏蔽该警告，也没有修改 SDK ABI。

## 验证与待办

`tests/test_core.py` 对同一份 C 核心执行调度对照、检索、词形歧义、大小写、前缀限制、损坏文件和输入边界测试；可额外抽查真实词库：

```powershell
python tests/test_core.py --library output/words_core.dll --reference <固定版本Py-FSRS目录> --dictionary output/dictionaries/library.wdb --csv output/source/ecdict.csv
```

Windows 主机测试库使用已有的 Visual Studio 开发命令行构建：

```text
cl /std:c11 /O2 /MT /LD core\words_fsrs.c core\words_dictionary.c /Fo:output\ /Fe:output\words_core.dll /link /EXPORT:words_fsrs_review /EXPORT:words_dictionary_open /EXPORT:words_dictionary_search /EXPORT:words_dictionary_entry
```

系统内置 MiSans Normal 覆盖四级词库使用的全部音标字符，包括 `U+04D9` 和 `U+0454`；词库原文不作替换。完整词库仍有少量 MiSans 未覆盖的字符（`U+E143`、`U+02B9`、`U+2011`），需要结合词条原文单独核对。

`tests/test_learning.py` 使用生产服务、队列策略和存储代码，文件系统及 RTOS 调度入口由主机测试替身提供。覆盖未评分恢复、重复评分拒绝、跨日额度、复习不受新词额度限制、收藏/暂停独立性、取消收藏后的未学队列、导入空间不足、保存失败重试、多块重载、大小写身份区分、9,217 条记录重载、扩容失败重试和资源释放。该测试没有模拟真实 RTOS 抢占、LittleFS 断电行为或 LVGL 控件绘制。

在 Visual Studio 开发命令行中构建并运行：

```text
cl /std:c11 /utf-8 /O2 /MT /LD /W4 /D_CRT_SECURE_NO_WARNINGS /Itests\shim /Icore tests\service_harness.c words_store.c core\words_learning.c core\words_fsrs.c core\words_dictionary.c /Fo:output\ /Fe:output\words_learning.dll
python tests/test_learning.py --library output/words_learning.dll
```

上板验证：

1. 安装更新后检查首页与系统“应用设置”入口；调整额度后退出重进。
2. 连续评分、快速重复点击、评分后立即返回，检查只计一次并能恢复。
3. 展开答案、进入完整释义再返回，检查展开状态和页面切换全刷；同页答案与换词仍走平台局刷策略。
4. 收藏和移除单词、暂停和恢复学习、切换词书，检查旧进度不被清空。
5. 导入词书时返回、空间不足、TF 拔出后重新插卡加载，检查已经保存的学习数据和已导入内容仍可读。
6. 长时间停留不操作，检查没有页面轮询重绘；退出后检查模块与 RAM1 内存回收。

尚未实现词频排序、相邻多词预读和音标补充字形。当前默认 FSRS 参数固定，设置页仅开放每日额度和词书选择。主机测试和模块编译不等同于已通过上述上板验证。
