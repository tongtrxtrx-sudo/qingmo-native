# Code signing policy

更新日期：2026-09-07。**当前没有已配置的发布签名；SignPath Foundation 尚未申请或获批，项目尚未获得其免费签名赞助。** 本文的后续政策为申请准备草稿，不能作为任何现有文件已经签名的证明。

计划申请 [SignPath Foundation](https://signpath.org/) 的开源项目免费签名。是否接纳项目由该机构审核决定；开源或采用 MIT 许可证不代表自动符合资格。申请和配置时必须重新核对 [官方条款](https://signpath.org/terms)。

## 启用前需要完成

- 公开源码、功能说明和可核验的发行记录，确认项目与所有随附组件符合适用条件。
- 在 GitHub 和 SignPath 为参与团队启用多因素认证，确定并公开实际作者、审查者和签名批准者及账号链接。
- 配置由仓库源码和构建脚本自动生成、来源可核验的 CI 产物；限制签名对象的产品名称与版本元数据。
- 由受信任团队成员人工批准每次签名请求；核对提交、构建、测试结果及拟发布的文件。
- 获批并启用后，再在主页和下载页加入官方要求的署名、适用徽标、实际团队角色以及本政策和 [隐私说明](PRIVACY.md) 链接。

以上准备依据 [SignPath Foundation 条款](https://signpath.org/terms)。目前实际签名角色尚未配置，不能把账号持有人默认列为已经授权的批准者。

## 获批后拟采用的政策

只为轻墨自身维护的源码经受控构建产生的 `QingmoNative.exe` 请求签名。本次上游 `Scintilla.dll`、`Lexilla.dll` 的 `Get-AuthenticodeSignature` 检查均为 `Valid`；保留其已有有效签名、来源和许可，不使用本项目订阅给上游二进制代签。打包范围必须与审批的产物一致。

签名批准者在批准前核对对应提交及自动构建来源、测试是否完成和文件清单。Pull Request 和来自分支的非受信任构建不得访问签名凭据。发布说明将区分签名状态、验证状态和已知限制，并提供源码提交及产物校验信息。

获批后按官方要求使用的署名草稿为：

> Free code signing provided by [SignPath.io](https://about.signpath.io/), certificate by [SignPath Foundation](https://signpath.org/).

该句仅为未来配置模板，当前不表示赞助成立。实际团队角色和徽标应在批准后填写或启用。签名不能替代功能测试，也不意味着程序在所有 Windows 安全策略下必然可以运行。
