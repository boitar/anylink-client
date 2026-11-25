# AnyLink 项目结构说明

## 1. 项目定位

AnyLink Secure Client 是一个基于 Qt 6、C++14 和 qmake 的桌面 SSL VPN 客户端。仓库内主要是 GUI 和本地 JSON-RPC WebSocket 客户端，不包含 VPN 隧道和系统网络接口实现。

VPN 隧道由外部 `tlslink/sslcon` release 提供：

- `vpnagent`：以 macOS launchd/system service 方式运行的后台服务，监听 `127.0.0.1:6210`。
- `sslcon`：命令行客户端，随 macOS 包一起分发，但 AnyLink GUI 当前只使用 `vpnagent` 的 RPC 接口。

当前代码基线：

| 项目 | 当前值 |
| --- | --- |
| 分支 | `main` |
| HEAD | `3a69a3e`（`test SignPath`） |
| GUI 版本 | `0.9.6` |
| Qt 模块 | `core gui websockets widgets` |
| C++ 标准 | C++14 |
| macOS 最低版本 | `11.0` |
| macOS 发布架构 | Apple Silicon arm64 |

## 2. 目录树

```text
anylink-client/
├── src/                         Qt 客户端 C++ 源码、头文件和 Designer UI
├── 3rdparty/                    Git submodule（当前未展开）
│   ├── SingleApplication/       单实例、secondary 启动通知
│   └── qtkeychain/              Keychain/凭据存储适配
├── resource/                    Qt 资源、界面样式、平台启动文件
│   ├── mac/                     Info.plist 和 macOS 图标
│   └── linux/                   .desktop 文件
├── assets/                      状态图标和应用图标
├── images/                      README 截图和帮助图片
├── i18n/                        Qt 翻译源文件和编译后的 .qm
├── installer/                   Qt Installer Framework 配置、脚本和包元数据
├── .github/workflows/           GitHub Actions 发布流水线
├── anylink.pro                  qmake 工程和平台输出目录配置
├── resource.qrc                 内嵌样式和安全提示文档
├── icons.qrc                    内嵌状态/应用图标
├── i18n.qrc                     内嵌中文翻译
├── README.md                    用户安装、平台和安全说明
└── DynamicSplitTunneling.md     动态分流功能说明
```

## 3. 源码模块

| 文件 | 职责 | 关键接口或状态 |
| --- | --- | --- |
| `src/main.cpp` | 应用启动、配置目录、翻译、单实例和主窗口创建 | `SingleApplication`、`instanceStarted`、`setQuitOnLastWindowClosed(false)` |
| `src/anylink.{h,cpp}` | 主窗口、Dock/托盘菜单、配置、VPN 状态和 RPC 编排 | `closeEvent`、`showEvent`、`afterShowOneTime`、`connectVPN`、`disconnectVPN` |
| `src/macdockiconhandler.{h,mm}` | macOS Dock reopen 回调的 Objective-C++ 适配 | `applicationShouldHandleReopen:hasVisibleWindows:` |
| `src/jsonrpcwebsocketclient.{h,cpp}` | JSON-RPC 2.0 over WebSocket 的异步调用和回调表 | `ws://127.0.0.1:6210/rpc`、请求 ID |
| `src/profilemanager.{h,cpp}` | VPN profile 增删改、JSON 文件和 Keychain 异步读写 | `profile.json`、`keyRestored` |
| `src/keychainclass.{h,cpp}` | qtkeychain 的读写删除薄封装 | service 名 `keychain.anylink` |
| `src/configmanager.{h,cpp}` | 客户端配置 JSON 读写和默认值 | `config.json`、`lastProfile`、`autoLogin`、`minimize` |
| `src/detaildialog.{h,cpp}` | VPN 路由和流量统计详情窗口 | `status`/`stat` RPC |
| `src/textbrowser.{h,cpp}` | 日志和安全提示的文本/Markdown 查看器 | `vpnagent.log`、Qt resource |
| `src/loading.{h,cpp}` | 连接过程中的点状加载动画 | `QTimer` 驱动重绘 |
| `src/*.ui` | Qt Designer 生成的界面定义 | 主窗口、profile、详情、文本查看器 |

### 主窗口初始化顺序

1. `main.cpp` 设置应用名、配置目录和临时目录，构造 `SingleApplication`。
2. 创建 `AnyLink` 并立即 `show()`。
3. `AnyLink::showEvent()` 在首次显示后延迟 50 ms 调用 `afterShowOneTime()`。
4. `afterShowOneTime()` 创建 QAction、`QSystemTrayIcon`/Dock 菜单、配置绑定、profile 管理器和详情窗口。
5. 创建 `JsonRpcWebSocketClient`，一次性连接 `ws://127.0.0.1:6210/rpc`。
6. WebSocket 连接成功后发送 `config`；用户连接时发送 `connect`，成功后周期性请求 `status`，连接断开时处理 `DISCONNECT` 或 `ABORT` 回调。

首次显示相关初始化依赖 `trayIcon == nullptr` 作为一次性标记。后续窗口显示不会重新创建 RPC 或菜单，这也是窗口生命周期改造时需要显式化的边界。

## 4. 窗口和单实例生命周期

当前项目有两层“重新显示”入口：

- macOS Dock 点击：`MacDockIconHandler` 发出 `dockIconClicked`，连接到 `AnyLink::showNormal()`。
- 重复启动应用：`SingleApplication` 将新进程判定为 secondary，通知 primary 的 `instanceStarted`，primary 连接到 `AnyLink::showNormal()`；secondary 随后退出。

当 VPN 已连接时，`AnyLink::closeEvent()` 将窗口 `hide()` 而不是退出，以便隧道继续运行；未连接时同一个事件直接调用 `qApp->quit()`。`actionQuit` 通过调用 `close()` 复用这套逻辑，再根据连接状态补充 `qApp->quit()`。

## 5. 配置、凭据和日志

| 数据 | 当前位置/来源 | 说明 |
| --- | --- | --- |
| GUI 配置 | `QStandardPaths::AppConfigLocation/config.json` | JSON，保存最近 profile 和连接选项 |
| VPN profiles | `QStandardPaths::AppConfigLocation/profile.json` | 密码字段写入前会移除，密码由 Keychain 保存 |
| Keychain | qtkeychain service `keychain.anylink` | key 为 profile 名 |
| agent 日志 | UI 配置成功后传入 `QStandardPaths::TempLocation`，文件名 `vpnagent.log` | UI 的“查看日志”直接读取该路径 |
| launchd 初始输出 | 外部 `kardianos/service` 默认使用 `/var/log/sslcon.out.log` 和 `/var/log/sslcon.err.log` | agent 还未收到 GUI 日志配置时使用 |
| macOS agent 服务 | 外部 `sslcon` 默认服务名 `sslcon`，通常为 `/Library/LaunchDaemons/sslcon.plist` | 不是名为 `vpnagent` 的 launchd 服务 |

## 6. 构建和打包

### qmake 工程

`anylink.pro`：

- 引入 `SingleApplication` 和 `qtkeychain` 两个 submodule。
- macOS 输出 `out/bin/AnyLink.app`，使用 `resource/mac/Info.plist` 和 `anylink.icns`。
- Windows 输出 `out/bin`，Linux 输出 `out/opt/anylink/bin`。
- UI、资源和翻译通过 `FORMS`/`RESOURCES` 编入程序。

### macOS 发布流水线

`.github/workflows/release.yml` 的 macOS 分支当前流程是：

1. qmake + make 编译 arm64 GUI。
2. `macdeployqt AnyLink.app` 部署 Qt 依赖。
3. 对当时的 `AnyLink.app` 执行一次 `codesign`。
4. 从 `tlslink/sslcon` 的 `latest` release 下载 `vpnagent` 和 `sslcon`，拷入 `AnyLink.app/Contents/MacOS`。
5. 用 `archivegen` 将整个 app 写入 Qt IFW root package。
6. 生成、签名并公证 installer app，再制作 DMG。

第 3、4 步的顺序以及 agent 下载未固定版本/校验和，是当前 macOS 发布可靠性的重要风险，详见 [macOS 问题分析](macos-issues-analysis.md)。

## 7. 安装器行为

`installer/packages/root/meta/component.js` 在 macOS 上：

- 将真实安装目录设置为 `~/AnyLink`。
- 在 `/Applications/AnyLink.app` 创建指向真实 app 的链接。
- 通过提权操作执行 `~/AnyLink/AnyLink.app/Contents/MacOS/vpnagent install`。
- 更新时尝试停止 `AnyLink`，已有安装时执行旧维护工具。

`installer/config/controller.js` 安装成功后直接启动真实安装目录中的 `AnyLink` 可执行文件，而不是通过 `/Applications` 链接启动。

## 8. 外部依赖边界

| 依赖 | 版本/来源 | 作用 | 是否纳入当前仓库源码 |
| --- | --- | --- | --- |
| Qt 6 | GitHub Actions 安装，当前 workflow 为 `6.11.1` | GUI、WebSocket、资源和构建 | 否 |
| SingleApplication | gitlink `a8da87d...` | 单实例 IPC | 以 submodule 引入，当前未展开 |
| qtkeychain | gitlink `53a8e6a...` | macOS Keychain 等凭据存储 | 以 submodule 引入，当前未展开 |
| sslcon | release `latest` | `vpnagent`、VPN 隧道和 RPC 服务 | 否 |
| Qt Installer Framework | workflow 下载 | 离线安装器/维护工具 | 否 |

构建和发布问题不能只通过编译 AnyLink GUI 验证；至少还要验证最终 app bundle 内的 agent、launchd plist、架构和代码签名。
