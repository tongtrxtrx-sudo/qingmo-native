# 轻墨 · Qingmo Native

Windows 10 / 11 x64 上的本地 Markdown 编辑器。使用 Win32、RichEdit 和 Scintilla 提供原生界面、基础正文编辑、源码编辑和阅读视图，不依赖浏览器、WebView2 或 Node.js。

本项目以 [MIT 许可证](LICENSE)开源。第三方组件保留各自许可证，见 [第三方声明](THIRD_PARTY_NOTICES.md)。

## 当前状态

这是 0.3 原生预览版本。2026-09-07 的 [Windows CI](https://github.com/tongtrxtrx-sudo/qingmo-native/actions/runs/34078950323) 已通过四组单元测试与全部 34 项窗口集成检查，包括代码块回车、查找、全屏和文件列表。该结果来自 GitHub 托管的 Windows 环境；开发电脑的最终启动验收和内存测量仍未完成。具体下载包以对应发布说明及随附的 `verification.json` 为准。

当前产物未签名。SignPath Foundation 免费签名尚未申请或获批，项目尚未获得签名赞助。有关计划与限制，见 [Code signing policy](docs/CODE_SIGNING.md)。

## 开始使用

发布包提供 Windows x64 便携 ZIP；已发布资产以 [Releases](https://github.com/tongtrxtrx-sudo/qingmo-native/releases) 页面实际内容为准。

1. 将 ZIP 完整解压到有写入权限的文件夹。
2. 保持 `QingmoNative.exe`、`Scintilla.dll`、`Lexilla.dll` 和 `licenses` 文件夹在同一目录层级。
3. 运行 `QingmoNative.exe`，打开随包附带的 `原生版开始.md`，或打开自己的 Markdown 文件。

无需安装服务，不自动更改文件关联。升级时解压到新的目录；退出程序后删除程序目录即可移除程序，自己的文档应另行保留。设置存于 `%LOCALAPPDATA%\QingmoNative\settings.ini`，需要完全清除设置时可在退出后删除该文件。

未签名程序可能被 Windows 安全策略阻止运行。本项目不要求关闭系统保护；当前包不能视为已经解决应用信誉或签名限制。

## 编辑能力

正文编辑支持段落、六级标题、粗体、斜体、删除线、行内代码、无语言标签的代码块、单层列表和简单引用。这是 Markdown 子集的基础所见即所得编辑，尚不提供输入 Markdown 标记后自动转换格式。正文粘贴使用纯文本，粘贴 Markdown 源码请切换到源码模式。

表格、链接、图片、任务列表、嵌套结构、HTML、数学及扩展语法、带语言标签的代码块会回退到阅读视图，选择编辑时进入源码模式。图片仅显示替代文字，不联网加载。超过 256 KiB 的文档使用源码编辑；阅读最多预览前 128 KiB。

正文未修改时，保存和视图切换保留原 Markdown 字节、换行和编码；正文修改后会重新生成 Markdown，标记与空白可能规范化。源码支持 UTF-8、UTF-8 BOM、UTF-16 LE/BE，并检测外部文件修改冲突。

| 操作 | 快捷键 |
| --- | --- |
| 新建 / 打开 / 保存 | Ctrl+N / Ctrl+O / Ctrl+S |
| 另存为 | Ctrl+Shift+S |
| 正文与源码切换 | F6 |
| 阅读视图 | F7 |
| 全屏专注 | F11 |
| 文件列表展开 / 收起 | Ctrl+Shift+E |
| 查找 | Ctrl+F |
| 粗体 / 斜体 | Ctrl+B / Ctrl+I |

完整操作说明见 [原生版使用说明](docs/NATIVE.md)。文档保存在用户选择的位置，应用无账户、广告或遥测，详情见 [隐私说明](docs/PRIVACY.md)。

## 从源码构建

在 Windows x64 上准备 PowerShell、`curl.exe` 和 `tar.exe`，然后在仓库根目录执行：

```powershell
./scripts/build-native.ps1 -Tests
./scripts/package-native.ps1 -SkipBuild
```

构建脚本准备 Zig 0.16.0、Scintilla / Lexilla 与 MD4C，下载归档使用固定 SHA-256 校验。首次构建需要联网；依赖详情以 `scripts/bootstrap-native.ps1` 为准。

程序输出到 `dist/native/QingmoNative.exe`，便携包输出到 `build/Qingmo-native-0.3-windows-x64.zip`。`-Tests` 运行文档、预览、滚动和 RichEdit 测试及原生窗口集成自检，集成报告为 `build/native/self-test.json`。测试必须在系统允许运行这些构建产物的环境中完成；仅编译成功不代表测试通过。

欢迎通过 [Issues](https://github.com/tongtrxtrx-sudo/qingmo-native/issues) 和 Pull Request 参与，提交前请阅读 [贡献说明](CONTRIBUTING.md)。
