# 轻墨原生版 0.3

面向 Windows 10 / 11 x64 的本地 Markdown 编辑器。界面与正文编辑使用 Windows 原生控件，不需要 WebView2、浏览器或 Node.js。

## 开始使用

完整解压压缩包后，运行 `QingmoNative.exe`。请把 `Scintilla.dll`、`Lexilla.dll` 保留在程序旁边。打开随附的 `原生版开始.md` 即可体验写作；自己的文章可以保存在任意有写入权限的文件夹。

这个版本提供原生所见即所得编辑、Markdown 源码编辑和阅读视图。保存的文件仍是 Markdown。

## 左侧文件列表

点击左侧顶部的“打开文件夹…”选择文件夹。选择后，这个按钮显示当前文件夹名称，再次点击可以换目录。列表显示子文件夹和 `.md`、`.markdown`、`.mdown`、`.txt` 文件；双击文件或按 Enter 打开，双击子文件夹进入，点击“上一级”返回。切换文件前会提示保存未保存的修改。

拖动列表右边缘可以调整宽度。顶部“文件”按钮或 Ctrl+Shift+E 收起/展开列表；全屏专注时自动隐藏。目录只按当前层读取，不递归扫描整个磁盘；非常大的目录最多检查 10000 个条目。新建或外部新增的文件可以重新选择文件夹刷新。

## 编辑与快捷键

| 操作 | 快捷键 / 入口 |
| --- | --- |
| 新建、打开、保存 | Ctrl+N、Ctrl+O、Ctrl+S |
| 另存为 | Ctrl+Shift+S |
| 源码与正文切换 | F6 |
| 阅读模式 | F7 |
| 全屏专注 / 退出 | F11；Esc 先关闭查找，再退出专注 |
| 查找 | Ctrl+F，输入后自动定位；Enter / Shift+Enter 下一处 / 上一处 |
| 粗体、斜体 | Ctrl+B、Ctrl+I |
| 标题、列表、引用、代码 | “格式”菜单 |
| 字号缩放、正文宽度、主题 | “排版”菜单；Ctrl + / -；Ctrl+Shift+T |

正文编辑支持段落、六级标题、粗体、斜体、删除线、行内代码、没有语言标签的代码块、单层有序/无序列表、简单引用。代码块内回车的保留结构修复已编译，最终运行回归尚待验证。粘贴使用纯文本；Markdown 源码请在源码模式粘贴。格式通过菜单或快捷键应用，暂不提供键入 Markdown 标记后自动转为格式的功能。

表格、链接、图片、任务列表、嵌套结构、HTML、数学/扩展语法、含语言标签的代码块暂不支持正文编辑。打开这些文件时自动进入原生阅读，点击“编辑”会转到源码，原文完整保留。图片显示替代文字，不联网加载。超过 256 KiB 使用源码编辑；阅读预览最多显示前 128 KiB。

未修改正文时，保存和视图切换保留原 Markdown 字节、换行和编码；修改正文后会生成等效的 Markdown，标记和空白可能规范化。源码支持 UTF-8、UTF-8 BOM、UTF-16 LE/BE，以及外部文件修改冲突检测。阅读/正文切换保留撤销；从已修改的源码重新生成正文时会重新建立正文撤销历史。

显示设置和最近的文件夹保存在 `%LOCALAPPDATA%\QingmoNative\settings.ini`，与 WebView2 版分开。

文件不会上传到云端。程序不安装服务，也不会自动修改 Markdown 的系统默认打开方式。

## 文件与版本

原生版使用独立名称 `QingmoNative.exe`。开发目录中的发布文件位于 `dist/native`，可以和已有版本并存。升级时请解压到新的文件夹，保留自己的文章。

`licenses` 文件夹包含随附组件的许可证，请随程序一起保留。

## 从源码构建

在项目根目录运行 PowerShell：

```powershell
./scripts/build-native.ps1 -Tests
./scripts/package-native.ps1 -SkipBuild
```

首次构建会准备 Zig、Scintilla、Lexilla 和 MD4C；缺失的下载归档会校验 SHA-256。构建结果为 `dist/native/QingmoNative.exe`，测试报告位于 `build/native/self-test.json`。独立单元测试覆盖文档处理、原生富文本、预览和滚动行为，程序自检覆盖原生界面集成。测试需在系统允许运行构建产物的环境完成。

发布压缩包为 `build/Qingmo-native-0.3-windows-x64.zip`。打包使用明确的文件清单，只包含程序、两个运行库、许可证及本版说明与示例，不会收集发布目录内的其他文件。

## 本机验证状态（2026-09-07）

C++17 Release 构建完成。此前文档读写、Markdown 阅读、滚动算法、真实 RichEdit 富文本转换测试通过；窗口集成的正文编辑/保存、源码切换、查找、全屏、文件列表打开及收起等检查通过。最后新增的代码块回车修复尚未完成运行回归；最终版本不能视为已经完整验收。

最终程序尚未代码签名。本机 Windows 应用程序控制对部分新构建记录了 Code Integrity 3033/3077 拦截，因此最终完整启动验收和内存测量未完成。日志对应 Windows 智能应用控制（VerifiedAndReputableDesktop），它没有针对单个程序的允许按钮；保持该保护开启时需要受信任代码签名或被微软应用信誉服务认可。本机没有现成代码签名证书。此包不会修改系统安全策略。微软说明：https://support.microsoft.com/en-US/Windows/Security/Threat-Malware-Protection/smart-app-control-frequently-asked-questions
