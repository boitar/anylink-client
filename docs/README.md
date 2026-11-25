# AnyLink 文档索引

本文档基于仓库 `main` 分支当前提交 `3a69a3e`（版本号 `0.9.6`）整理。文档区分仓库内的 Qt 客户端代码、安装器和发布流水线，以及由发布流水线下载并打包的外部 `sslcon/vpnagent` 服务。

## 文档

- [项目结构说明](project-structure.md)：目录、模块职责、运行时数据流、构建和打包入口。
- [macOS 问题分析](macos-issues-analysis.md)：窗口无法重呼出和 `vpnagent` 连接失败的源码证据、根因分级和现场排查方式。
- [macOS 改造方案与任务拆解](macos-remediation-plan.md)：目标架构、实施顺序、验收标准和回归矩阵。

## 阅读边界

`3rdparty/SingleApplication` 和 `3rdparty/qtkeychain` 是 Git submodule，当前工作树只记录了它们的 gitlink，没有展开源码。新环境需要使用 `git clone --recurse-submodules` 或执行 `git submodule update --init --recursive` 后才能完整构建。

`vpnagent` 和 `sslcon` 不在本仓库源码中，而是在 `.github/workflows/release.yml` 中从 `tlslink/sslcon` 的 `latest` release 下载后放入安装包。因此，服务行为、macOS launchd 注册和服务二进制签名既需要检查本仓库，也需要检查外部依赖和最终安装产物。
