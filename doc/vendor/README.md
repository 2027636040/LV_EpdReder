# 浏览器依赖

脚本直接随仓库分发，知识库不依赖 CDN。这里保存未修改的上游发行文件。

| 组件 | 固定版本 | 本地文件 | 许可证 |
| --- | --- | --- | --- |
| [Marked](https://marked.js.org/) | 18.1.0 | `marked.umd.js` | MIT，见 `LICENSE-marked` |
| [DOMPurify](https://github.com/cure53/DOMPurify) | 3.4.16 | `purify.min.js` | Apache-2.0 或 MPL-2.0，见 `LICENSE-dompurify` 和 `LICENSE-MPL-dompurify` |

## 来源与校验

Marked 来自 [npm 发行包](https://registry.npmjs.org/marked/-/marked-18.1.0.tgz) 的 `package/lib/marked.umd.js`。

```text
SHA256 f424dcb508fdf93e0137a970cfce8f3207ea2e3f37eca5f7556a52875683632a
```

DOMPurify 来自 [npm 发行包](https://registry.npmjs.org/dompurify/-/dompurify-3.4.16.tgz) 的 `package/dist/purify.min.js`。

```text
SHA256 2c90a9b46d6463f26038a29b686e82bc91de01fdac9d5229e7cfe3b360134ea2
```

升级时复制对应发行文件和许可证，更新版本与校验值，并重新运行知识库测试及浏览器检查。开发者工具需要的源映射不随网页分发；运行页面不需要源映射。
