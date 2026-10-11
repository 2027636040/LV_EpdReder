# 开发文档

在浏览器中打开[网页知识库](../doc/index.html)，可以按主题阅读并搜索以下文档。

## 阅读路径

首次接触工程，先阅读 [平台架构](ARCHITECTURE.md)，再按工作内容选择以下入口。

| 工作内容 | 主要文档 |
| --- | --- |
| 编译平台、选择板型和预装应用 | [构建](BUILDING.md)、[LCD DevKit](../boards/sf32lb57-lcd-gyd7n6_epd/README.md) |
| 在仓库外开发应用 | [独立开发包](APP_SDK.md)、[应用开发](APPLICATION_DEVELOPMENT.md) |
| 页面与后台任务 | [应用生命周期](APPLICATION_LIFECYCLE.md)、[hello 设计](../modules/hello/DESIGN.md) |
| 从文件管理器打开应用内容 | [内容交接](CONTENT_HANDLERS.md) |
| 修改信息看板 | [模板与桌面预览](BOARD_TEMPLATES.md) |
| 安装位置和历史数据迁移 | [应用开发](APPLICATION_DEVELOPMENT.md)、[存储迁移](STORAGE_MIGRATION.md) |
| 定位动态模块异常 | [故障取证](DYNAMIC_APP_DEBUGGING.md)、[故障验证](DYNAMIC_APP_DIAGNOSTICS_VALIDATION.md)、[恢复入口](RECOVERY.md) |
| 维护 SD 组件 | [组件说明](../components/sdcard/README.md)、[SD 实施记录](SD_CARD_COMPONENT_TODO.md) |
| 形成产品和社区发行组合 | [参考组合与发布](RELEASING.md) |
| 后续能力设计 | [平台演进](PLATFORM_ROADMAP.md)、[技术储备](TECHNICAL_RESERVES.md) |

## 应用设计

应用 README 说明使用与构建，DESIGN 说明内部职责、执行流程、数据和资源生命周期。

| 应用 | 使用说明 | 设计 |
| --- | --- | --- |
| 天气 | [README](../modules/weather/README.md) | [DESIGN](../modules/weather/DESIGN.md) |
| 书架 | [README](../modules/books/README.md) | [DESIGN](../modules/books/DESIGN.md) |
| 图库 | [README](../modules/gallery/README.md) | [DESIGN](../modules/gallery/DESIGN.md) |
| 单词 | [README](../modules/words/README.md) | [DESIGN](../modules/words/DESIGN.md) |
| 微信读书 | [README](../modules/weread/README.md) | [DESIGN](../modules/weread/DESIGN.md) |
| 信息看板 | [README](../modules/board/README.md) | [DESIGN](../modules/board/DESIGN.md) |
| hello | [开发示例](APPLICATION_DEVELOPMENT.md) | [DESIGN](../modules/hello/DESIGN.md) |

## 实施与历史记录

[平台实施清单](PLATFORM_IMPLEMENTATION_TODO.md) 记录当前建设项目的交付状态；[验收记录](PLATFORM_VALIDATION.md)提供测试、构建产物和上板检查入口。
[早期平台实施方案](APPLICATION_PLATFORM_PLAN.md)、[随卡应用清单](PORTABLE_APPS_TODO.md)、
[阅读性能方案](READER_PERFORMANCE_PLAN.md) 及各专项 TODO 保留阶段结果。历史镜像大小、构建标识和测试记录不作为当前二进制身份。

## 文档维护约定

- 架构文档说明系统边界和公共职责，应用文档说明应用内部设计，避免多处维护同一份操作步骤。
- 公开接口的调用线程、对象所有者、有效期和错误行为与头文件保持一致。
- 方案区分已有实现与后续设计；验证记录区分主机测试、目标编译和实机运行。
- 示例使用仓库根目录的相对命令，路径和安装目录以当前源码为准。
- 资源说明保留来源和授权，记录数据版本及再生成入口，不包含实际账号凭据。
