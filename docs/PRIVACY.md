# 隐私说明

适用于本仓库的轻墨原生版，更新日期：2026-09-07。

轻墨是本地 Markdown 编辑器，不设账户，不包含广告、使用统计遥测或自动上传文档的功能。程序在用户选择的位置读取和保存文件；正文和阅读视图中的图片显示替代文字，不联网获取图片。

显示设置及最近打开的文件夹路径保存在 `%LOCALAPPDATA%\QingmoNative\settings.ini`。该文件可能包含本机目录信息，不由轻墨上传。关闭程序后可删除此文件以清除保存的设置；用户文档不会因此删除。

如果文件位于 OneDrive、其他同步文件夹或网络盘，读取、写入和同步行为还受 Windows、相应存储服务及用户配置影响。轻墨不管理这些外部同步服务。Windows 自身的安全检查、错误报告等功能也由系统及其设置管理。

从源码构建时，依赖准备脚本会连接脚本列出的上游站点下载工具和开源组件。这是开发构建行为，区别于编辑器日常运行。访问仓库、下载发行版和提交问题时，GitHub 按其 [隐私声明](https://docs.github.com/en/site-policy/privacy-policies/github-general-privacy-statement)处理平台数据。

反馈由用户自愿通过公开的 [GitHub Issues](https://github.com/tongtrxtrx-sudo/qingmo-native/issues)提交，提交内容可能对公众可见。请先移除文档、截图或日志中的个人信息和机密。项目维护者：[tongtrxtrx-sudo](https://github.com/tongtrxtrx-sudo)。本说明描述当前实现，不构成对操作系统或外部服务行为的保证。
