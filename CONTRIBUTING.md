# 贡献说明

欢迎报告问题、完善中文说明或提交原生编辑功能的改进。项目维护者为 [tongtrxtrx-sudo](https://github.com/tongtrxtrx-sudo)。

## 报告问题

在 [Issues](https://github.com/tongtrxtrx-sudo/qingmo-native/issues) 说明 Windows 版本、轻墨版本或提交、复现步骤、预期与实际结果。涉及文档转换时，提供去除敏感内容的最小 Markdown 示例，并说明使用正文、源码还是阅读视图。不要上传私人文档、密钥或包含个人路径的完整日志。

安全策略拦截、编译失败和编辑器行为错误应分别记录。测试没有运行时，请明确说明，不能仅以编译成功标记验证通过。

## 开发和验证

使用 Windows x64、PowerShell、`curl.exe` 和 `tar.exe`，在根目录运行：

```powershell
./scripts/build-native.ps1 -Tests
```

构建脚本会准备固定版本的依赖。测试包含文档处理、Markdown 预览、滚动算法、RichEdit 转换和原生界面自检。窗口集成结果位于 `build/native/self-test.json`。

围绕实际行为添加有意义的测试，特别关注 Markdown 数据保留、撤销、外部文件冲突、超大文档和不支持结构的回退。涉及界面的修改，还应手动检查文件打开和保存、F6 / F7 切换、查找、F11 专注及 Ctrl+Shift+E 文件列表。

## 提交更改

每个 Pull Request 聚焦一个问题，说明改动后的行为、验证结果及尚未验证的限制。代码和提交内容不应包含本地工具缓存、构建产物、私人文档、签名密钥或访问令牌。新增依赖应说明用途、来源和许可证，并更新第三方声明与打包清单。

提交的原创贡献按本项目 MIT 许可证提供；引入第三方内容时保留原始许可证和版权。外部贡献经维护者审查后合入。签名发布需要另行配置和人工批准，普通 Pull Request 不应获得发布或签名凭据。
