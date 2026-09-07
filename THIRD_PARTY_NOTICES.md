# 第三方组件与许可证

项目自身代码采用根目录的 MIT 许可证；该许可证不替代第三方组件各自的许可。构建使用的组件及再分发声明如下，完整原文随程序保存在 `licenses` 文件夹。

| 组件 | 用途 / 来源 | 许可证与发布包文件 |
| --- | --- | --- |
| Scintilla | 原生源码编辑控件；来自 Scintilla 官方 SciTE 5.6.6 归档 | Neil Hodgson 的 Scintilla 许可；`licenses/Scintilla.txt` |
| Lexilla | 源码语法着色；来自同版官方 SciTE 归档 | Neil Hodgson 的 Lexilla / Scintilla / SciTE 许可；`licenses/Lexilla.txt` |
| MD4C 0.5.3 | Markdown 解析 | MIT，Copyright © 2016–2024 Martin Mitáš；`licenses/MD4C.txt` |
| LLVM libc++ | C++ 标准库运行时代码 | Apache-2.0 with LLVM Exceptions，保留上游完整附加声明；`licenses/libcxx.txt` |
| LLVM libc++abi | C++ ABI 运行时代码 | Apache-2.0 with LLVM Exceptions，保留上游完整附加声明；`licenses/libcxxabi.txt` |
| LLVM libunwind | 栈展开运行时代码 | Apache-2.0 with LLVM Exceptions，保留上游完整附加声明；`licenses/libunwind.txt` |
| mingw-w64 | Windows 目标运行时 / 支持代码 | 上游 `COPYING` 说明以 ZPL-2.1 为主，部分文件另标 Public Domain、BSD 或 LGPL；`licenses/mingw.txt` |

Scintilla 和 Lexilla 的许可要求保留版权和授权通知；其版权原文为 `Copyright 1998-2021 by Neil Hodgson <neilh@scintilla.org>`。原始通知位于准备依赖后的 `third_party/scintilla/License.txt` 与 `third_party/lexilla/License.txt`，构建时原样复制到发布包。MD4C 原文位于 `third_party/md4c/LICENSE.md`。

编译器为 Zig 0.16.0，仅用于构建，不包含在便携包中。上述运行时许可由 `scripts/build-native.ps1` 从所用 Zig 发行包复制；即使运行时代码静态链接，也保留这些文件。此清单并不声称二进制链接了所有列出的运行时对象，完整许可原文用于保留发行工具链提供的声明。

依赖下载地址、版本和 SHA-256 位于 `scripts/bootstrap-native.ps1`。Scintilla 与 Lexilla 的 DLL 来自上游预编译归档；本次依赖的两个 DLL 已通过 `Get-AuthenticodeSignature` 检查，状态均为 `Valid`。轻墨保留其上游有效签名，不申请代签，也不将其宣称为自行开发的二进制。

重新分发时请保留根目录 `LICENSE`、本说明及发布包 `licenses` 的全部内容。修改或更换依赖时，应同步核对来源和许可证；上游许可原文优先于这里的摘要。
