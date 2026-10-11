# 网页知识库

在浏览器中打开 [index.html](index.html)，即可离线阅读、搜索仓库文档。网页包含平台架构、构建部署、应用开发、各应用设计、诊断恢复、技术储备和实施验证记录。

## 阅读

- 左侧目录按主题组织，右侧目录定位当前文档章节。
- 顶部搜索支持标题和正文，按 `Ctrl+K` 或 `Command+K` 打开，按 `Esc` 关闭。
- 文档地址可以复制；浏览器前进、后退保持正常导航。
- “Markdown 源文件”打开仓库中的原始文档。正文中的源码链接按原文位置解析。
- 窄屏使用左上角目录按钮。代码块和表格可以横向滚动。

网页的渲染库、内容和样式均在本地，不需要 CDN、Node.js 或网络连接。单独复制 `doc/` 也能阅读打包的正文；若要继续访问原始 Markdown、源码和其他附件，需要保留完整仓库目录结构。

## 更新内容

正文继续在 `docs/`、`modules/` 及组件目录的 Markdown 中维护。`catalog.json` 只定义分类、导航标题和源文件路径，`assets/content.js` 是自动生成的离线内容包，不直接编辑。

在仓库根目录运行：

```powershell
python doc/build.py
python doc/build.py --check
```

修改 Markdown 后重新生成内容包，并将正文与内容包一起提交。新增 `docs/` 文档时需要加入 `catalog.json`；生成器会报告尚未编入目录的文档。新增应用或组件文档也应在目录中登记。

生成过程只依赖 Python 3.10 或更高版本的标准库，不下载资源，不执行固件构建。相同输入产生相同输出；文件摘要用于追踪正文是否变化，不代表固件构建身份。

## 本地预览与验证

直接双击 `index.html`，或在仓库根目录启动本地服务器：

```powershell
python -m http.server 8765 --bind 127.0.0.1
```

浏览器访问 `http://127.0.0.1:8765/doc/`。静态托管也使用仓库目录布局，入口为 `/doc/`，这样正文中的源码链接可继续访问。

自动测试：

```powershell
python -m unittest discover -s doc/tests -p "test_*.py"
node --test doc/tests/core.test.cjs
python doc/build.py --check
```

测试覆盖生成一致性、目录覆盖、路径边界、导航、章节标识和搜索。

## 前端依赖

Markdown 由 Marked 渲染，生成的 HTML 经 DOMPurify 清理后进入页面。固定版本、下载地址、许可证和校验值记录在 [vendor/README.md](vendor/README.md)。第三方脚本和许可证随仓库保存，升级时应一并更新并重新验证。

网页只提供文档阅读与搜索，不执行文档中的命令，也不提供登录或云端同步。
