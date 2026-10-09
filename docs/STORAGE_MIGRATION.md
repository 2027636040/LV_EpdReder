# 内部 LittleFS 与旧数据迁移

## 当前布局

适用板型：`dpi-hdk_lb57gyd7n6_epd_hcpu`，NOR 基址 `0x12000000`，总容量 16 MiB。

| 区域 | 起始地址 | 容量 |
|---|---|---:|
| 启动表、校准、Bootloader | 0x12000000 | 128 KiB |
| 主程序 | 0x12020000 | 2 MiB |
| 只读资源 | 0x12220000 | 8 MiB |
| ACPU 预留 | 0x12A20000 | 128 KiB |
| BLE KVDB | 0x12A40000 | 16 KiB |
| 波形 | 0x12A44000 | 256 KiB |
| 内部 LittleFS | 0x12A84000 | 5,616 KiB |

独立 OTA 程序预留区已删除，没有实现 OTA 可执行程序，也没有启用 DFU_V2。
当前启动表继续启动主程序。SDK 指针及已有修复保持不变。
GBK 双向码表共 174,344 字节，改由项目链接脚本放在资源区，数据和调用方式不变。

内部 `/flash` 使用 SDK LittleFS 2.4、磁盘格式 2.0；TF `/sdcard` 继续使用 FatFs。
NOR 读/写/擦除粒度为 32/256/4096 字节，缓存 256 字节，运行时元数据磨损均衡周期 500。
挂载失败不会触发自动格式化。FS 类型注册数量为 4：devfs、romfs、elm、lfs。

## 文件与写入规则

| 内容 | 路径 |
|---|---|
| 应用程序、资源 | `/flash/apps/<id>/` |
| 应用持久化数据 | `/flash/data/<id>/` |
| 可重建缓存 | `/flash/cache/<id>/` |
| 系统设置 | `/flash/system/settings.bin` |
| 天气配置 | `/flash/data/weather/config.bin` |
| 天气缓存 | `/flash/cache/weather/current.bin` |
| 阅读位置 | `/flash/data/reader/{flash,sd}-<路径散列>.a/b` |
| 分页缓存 | `/flash/cache/reader/{flash,sd}-<路径散列>.idx` |
| 字体 | `/flash/fonts/` |
| OTA 更新包 | `/flash/update/` |

应用通过 `storage_app_path()` 构造目录，不添加物理分区。
`storage_record_save/load()` 提供魔数、版本、长度和 CRC32 校验；写入采用同目录临时文件、
`fsync`、关闭、LittleFS 原子重命名。`storage_file_replace()` 供已有格式的文件使用。
文件事务复用存储互斥锁；LittleFS 自身也使用 SDK 的文件系统锁。
这些接口同步执行，较大的导入放在业务工作线程调用，不在中断中调用。

天气仍由原工作线程保存数据；先写与配置指纹绑定的缓存，最后提交配置。
中途掉电可能导致缓存失效，但不会把其他城市的数据作为当前城市缓存。
30 分钟后台同步、PAN 就绪后同步及 5 秒重试逻辑不变。

内部 TXT 没有可用的文件修改时间，因此扫描时按内容计算 FNV-1a 标识，用于历史和分页缓存失效判断。
TF TXT 继续使用原有大小和修改时间判断。旧 TF 阅读记录会在首次扫描时从卡内 `.epd_reader` 导入，
不删除卡内原记录。总页数完成后的底栏更新方式不变。

阅读字体优先使用内部文件。内部缺少所选字体时，从 TF 的 `/fonts/Song.ttf`、`Hei.ttf`、
`Kai.ttf` 或 `Monospace.ttf` 完整复制到内部并提交，再交给 Tiny TTF 打开。
导入失败仍使用内置字体；不保留指向 TF 字体的句柄。已有内部同名字体不会被自动覆盖。
拔卡只关闭 TF 书籍，内部书籍和已经导入的字体可以继续使用。

## 已使用 LittleFS 的设备调整地址

上一布局的内部 LittleFS 位于 `0x12984000`、容量 6,640 KiB，BLE 位于 `0x12940000`。
当前资源区会覆盖上述旧地址的一部分，烧录前需要导出 `/flash` 文件并保存完整 NOR 读回文件。
新文件系统缩小了 1 MiB，需按文件重建，不能把旧 `fs_root.bin` 原样写入新地址。
BLE 原始 16 KiB 数据需要迁移到 `0x12A40000`；未恢复时原有配对关系不能保证保留。

下节的 `migrate_storage.py` 专门解析更早的 FAT 布局，不适用于上述 LittleFS 布局。
普通下载清单不包含文件系统，但新资源镜像本身已经与旧文件系统重叠，不能用普通下载保留旧数据。
允许重新初始化时，构建增加 `STORAGE_IMAGE=1`，生成的下载清单会将初始 `fs_root.bin` 写到新地址。
文件恢复或初始化后，动态应用必须使用同一次新固件构建产生的安装包。

## 旧 FAT 设备升级前导出

新旧地址大幅重叠，必须先保存完整旧 Flash 读回文件，再构建恢复镜像。
不能先烧录新主程序/资源，然后指望从旧地址找回记录。以下步骤不执行整片擦除。

1. 使用本机 `SFTOOL_BIN` 指向的 sftool，或 PATH 中的 sftool。退出阅读后，让设备停止写入。
   读回类型为 SF32LB57、NOR，范围为 `0x12000000:0x01000000`。
   PowerShell 中先把 `$port` 设为实际串口、`$dump` 设为不存在的绝对输出文件路径：

   ```powershell
   & $env:SFTOOL_BIN -c SF32LB57 -p $port --after no_reset read_flash "${dump}@0x12000000:0x01000000"
   ```

   未设置 `SFTOOL_BIN` 时用 PATH 中的 `sftool` 替换调用前缀。不要在读回后继续使用旧固件写入数据。
   保留完整 16 MiB 文件，校验其长度并计算 SHA-256；该文件同时包含校准、配对和原始记录。

2. 在项目根目录、独立 Python 环境中安装迁移工具依赖：

   ```powershell
   python -m pip install -r tools/storage_migration_requirements.txt
   python tools/migrate_storage.py --dump $dump --out $backup
   ```

   `$backup` 是一个尚不存在的绝对目录，建议使用较短路径。
   工具只读原始镜像；使用 PyFatFS 提取旧 FAT 文件，按 CRC 和序号选择最新天气/设置记录。
   天气记录按实际 ARM `-fshort-enums` 布局解析：配置 392 字节、天气快照 716 字节、旧记录 1,124 字节。
   内部阅读位置保留字节偏移，并更新文件内容标识；分页索引可重新生成。
   原始 FAT 镜像和原阅读记录保留在备份目录，BLE 原始 16 KiB 单独保留。
   检查输出的 `weather`、`settings`、`reading_records`、`ignored_history` 是否符合旧设备情况。
   备份包含 API 密钥，不加入版本库。

3. 从备份构建新布局固件和恢复镜像：

   ```powershell
   . .\SiFli-SDK\export.ps1
   Set-Location project
   scons --board=dpi-hdk_lb57gyd7n6_epd_hcpu -j8 "STORAGE_IMPORT=$backup"
   ```

   构建核对备份文件清单及 SHA-256。生成的下载清单包括主程序、资源、启动表、Bootloader、波形、
   `/flash` 恢复镜像及迁移到新地址的 BLE 数据。使用构建目录内的原生下载脚本烧录，不能只更新主程序。
   不要再使用旧布局的下载脚本。确认恢复成功后保留原始备份。

## 后续构建

- 普通 `scons --board=... -j8` 生成 `fs_root.bin`，但下载清单不含 FS/BLE，保护已安装应用和用户数据。
- 初始镜像默认只包含 `disk/`，需要预装应用时显式指定 `PREINSTALL=weather,books` 等列表。
  已迁移设备通过 TF 安装完整应用包，不覆盖整个文件系统。
  具体步骤见 [天气应用资源](WEATHER_RESOURCES.md)。
- 空白设备或明确允许重新初始化的设备，使用 `STORAGE_IMAGE=1`，将 `disk/` 初始内容加入下载清单。
  这个选项会覆盖内部文件系统，不用于保留旧数据的升级。
- 迁移完成后重新执行普通构建，避免后续下载再次恢复旧快照。
- 应用单独构建和预装列表的完整命令见 [平台与应用构建](BUILDING.md)。
- 部署使用同一次固件构建产生的动态应用包。模块兼容检查依据 `app-profile.json` 中的
  `build_id`；单纯调整分区地址不一定改变这个标识，不能依靠兼容检查处理旧文件系统迁移。

镜像由 SDK 自带的 `tools/mklfsimg/mklfsimg.exe` 生成，不修改 SDK，不要求安装 Visual Studio。
该工具使用 Windows 系统 ANSI 文件名编码；备份包含非 ASCII 文件名且系统编码与 UTF-8 不一致时，
构建会报错，不生成文件名错码的迁移镜像。文件内容不转码。编码边界见 [平台与应用构建](BUILDING.md)。
SDK 工具的源文件完整路径限制为 255 个 UTF-8 字节，构建时会检查。

## 上板验证

1. 首次启动确认 `/flash` 为 LittleFS、`/sdcard` 为 FatFs，存储页容量与分区一致。
2. 从备份恢复后检查城市/API 配置、天气缓存、系统设置、BLE 配对和阅读字节位置。
3. 修改设置、同步天气、翻页后重启，确认记录恢复；正常下载固件后记录仍在。
4. 导入 TF 字体后拔卡，确认内部书籍继续阅读；TF 书籍退出后重插可以恢复位置。
5. 反复安装/卸载应用并热插拔，确认程序、持久化数据和缓存目录分离。
6. 写入途中复位、空间不足或文件校验失败时，确认旧配置仍可恢复，挂载失败不自动格式化。

编译和镜像生成不等同于设备迁移成功；真实备份恢复、掉电和热插拔仍需上板验证。

源码依据：`src/services/storage.c`、`storage_file.c`、`weather/weather_store.c`、
`src/ui/ui_settings_store.c`、`ui_bookshelf_data.c`、`ui_font.c`；SDK 的
`rtos/rtthread/components/dfs/filesystems/littlefs/dfs_lfs.c` 和 `tools/mklfsimg/`。
离线 FAT 读取接口参考 [PyFatFS 源码](https://github.com/nathanhi/pyfatfs/blob/master/pyfatfs/PyFatFS.py)。
