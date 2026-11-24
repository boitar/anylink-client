# macOS 问题分析

## 1. 分析基线和结论

分析基于仓库 `main` 当前提交 `3a69a3e`、GUI 版本 `0.9.6`，并对照当前发布 workflow 与 `tlslink/sslcon` 的服务实现。两个现象不是同一个故障：

1. “窗口无法呼出、重新启动时 Dock 闪一下”主要是 AnyLink 自己的 macOS Dock reopen 和单实例窗口激活链路有缺陷。
2. “无法连接到 vpnagent”是本地 agent 的可用性问题，但当前客户端把端口不可达、服务未注册、服务启动失败、架构/签名问题和瞬时启动时序全部显示成同一条“请重装”提示，导致重装无法针对性修复。

## 2. 现有行为证据

| 位置 | 现状 | 对故障的影响 |
| --- | --- | --- |
| `src/main.cpp:33-36` | macOS 使用 `SingleApplication` | 重复启动不会创建第二个窗口 |
| `src/main.cpp:54-60` | primary 创建并显示窗口，secondary 只通知 `instanceStarted` 后退出 | 如果 primary 没有正确激活，用户只看到 Dock 闪一下 |
| `src/main.cpp:59` | `setQuitOnLastWindowClosed(false)` | 隐藏/最小化后进程应继续存活，但不能代替窗口激活 |
| `src/anylink.cpp:86-97` | 已连接时关闭窗口只 `hide()`；未连接时直接 `qApp->quit()` | “关闭”同时承担隐藏和退出两种语义 |
| `src/anylink.cpp:99-105` | 首次显示后延迟初始化托盘/Dock 菜单 | 窗口生命周期依赖异步的一次性初始化 |
| `src/anylink.cpp:172-176` | macOS 使用 Dock 菜单，并监听 `dockIconClicked` | Dock 是 macOS 上主要的呼出入口 |
| `src/anylink.cpp:174-175` | Dock 回调最终只调用 `showNormal()` | 没有统一的 `show`、置前和激活操作 |
| `src/main.cpp:60` | secondary 通知最终也只调用 `showNormal()` | 重复启动依赖同一个不完整的激活路径 |
| `src/macdockiconhandler.mm:22-25` | 运行时替换 `applicationShouldHandleReopen:hasVisibleWindows:`，类型编码为 `B@:` | selector 实际还带应用对象和可见窗口参数，签名不匹配 |
| `src/macdockiconhandler.mm:18-19` | 回调固定返回 `false` | 主动禁止 macOS 的默认 reopen 行为 |
| `src/anylink.cpp:305-336` | 固定只连接 `ws://127.0.0.1:6210/rpc` | agent 不在监听时 UI 无法工作 |
| `src/anylink.cpp:306-313` | 任意 WebSocket error 都显示“请重装”，并禁用连接按钮 | 没有错误分类、重试或修复入口 |
| `.github/workflows/release.yml:202-207` | app 签名后才拷入 `vpnagent`/`sslcon` 并打包 | bundle 内新增二进制没有被该次 app 签名覆盖 |
| `installer/packages/root/meta/component.js:92-100` | 提权执行 `vpnagent install` | agent 安装/启动是安装成功的必要条件 |
| 外部 `sslcon/svc/service.go` | 服务名为 `sslcon`，安装后调用 `Start()` | launchd 服务不叫 `vpnagent` |
| 外部 `sslcon/svc/service.go` | `InstallSvc()` 只打印错误，不返回非零退出码 | 安装失败可能被安装器误报为成功 |

## 3. 问题一：窗口关闭/最小化后无法呼出

### 3.1 复现后的进程路径

以 VPN 已连接为例：

```text
AnyLink primary 启动
  -> 首次 showEvent
  -> 延迟创建 Dock 菜单和 native reopen handler
  -> WebSocket 连接 vpnagent
  -> 连接成功后 closeEvent/hide（配置 minimize 时也会发生）
  -> 进程继续存活，窗口不可见或处于 minimized

Dock 点击或再次启动
  -> macOS 调用 applicationShouldHandleReopen:
  -> 当前 native handler 发出信号，但返回 false
  -> AnyLink 只调用 showNormal，没有统一置前/激活
  -> 再次启动的进程被 SingleApplication 识别为 secondary 并立即退出
  -> Dock 只出现一瞬间，用户看不到窗口
```

### 3.2 根因分级

#### A. 直接根因：reopen delegate 禁用了系统默认行为（已确认）

`src/macdockiconhandler.mm` 将 `applicationShouldHandleReopen:hasVisibleWindows:` 替换为 `dockClickHandler`，但回调固定返回 `false`。该返回值的效果是禁止 macOS 的默认重新打开行为。项目虽然在回调里发 Qt 信号，却没有用完整、可靠的窗口激活流程补上被禁止的系统行为。

同时，`class_replaceMethod` 使用 `"B@:"` 作为 Objective-C 方法类型编码，而 selector 实际包含显式参数。处理一个带参数的 AppKit delegate 方法时，这个签名不完整，属于未定义行为，在不同 macOS/Qt 组合上可能表现为回调不生效、窗口不激活甚至进程异常。

#### B. 高概率放大因素：显示操作不包含置前和激活（已确认）

Dock 点击、Dock 菜单的 `Show Panel` 和 `SingleApplication::instanceStarted` 都最终只调用 `QWidget::showNormal()`。代码没有统一调用 `show()`、`raise()`、`activateWindow()`，macOS native 层的 `activateIgnoringOtherApps` 虽然已经在 `macdockiconhandler.mm:50-53` 提供，却没有被使用。

因此，窗口从 hidden/minimized 状态恢复时，即使 Qt 的可见状态变化，应用也可能仍不是前台 active app，用户仍然看不到窗口。

#### C. 生命周期设计缺陷：close 与 quit 语义混用（已确认）

`closeEvent` 通过 `m_vpnConnected` 选择隐藏或退出，`actionQuit` 又调用 `close()` 后再条件性调用 `qApp->quit()`。这使“用户关闭窗口”“连接后自动最小化”“菜单退出”共用同一个事件入口，容易在修复过程中互相影响。当前没有 `quitRequested` 状态，也没有独立的 `showMainWindow()`/`quitApplication()` 方法。

这不是 Dock 回调失效的唯一直接原因，但会让修复后的行为不稳定，应该一起拆开。

### 3.3 为什么再次启动只闪一下

这不是第二个 AnyLink 窗口启动后马上崩溃。`SingleApplication` 的默认行为是：发现已有 primary 后，将当前进程作为 secondary 通知 primary，然后调用 `exit(EXIT_SUCCESS)`。因此只要 primary 仍然存活，Dock 上的短暂闪烁就是预期的 secondary 退出表现；真正需要修复的是 primary 收到通知后的窗口恢复和激活。

### 3.4 不应作为唯一修复的方向

仅把 `QApplication::setQuitOnLastWindowClosed(false)` 提前到 `w.show()` 之前不能解决问题。当前 closeEvent 已经显式调用 `qApp->quit()` 或 `hide()`，而 `setQuitOnLastWindowClosed` 只影响 Qt 对最后一个窗口关闭的默认处理，不会修复 native reopen callback、窗口置前或 secondary 转发。

## 4. 问题二：无法连接到 vpnagent

### 4.1 实际连接链路

```text
AnyLink GUI
  -> QWebSocket::open(ws://127.0.0.1:6210/rpc)
  -> 本机 sslcon/vpnagent 的 HTTP/WebSocket RPC handler
  -> JSON-RPC config/connect/status/reconnect

安装器
  -> 将 app 安装到 ~/AnyLink/AnyLink.app
  -> 创建 /Applications/AnyLink.app 链接
  -> 提权执行 .../Contents/MacOS/vpnagent install
  -> 外部 service 库写入 /Library/LaunchDaemons/sslcon.plist
  -> launchctl load 并启动 sslcon 服务
```

外部 `vpnagent` 无参数运行时在 launchd 环境下进入 service mode，调用 `rpc.Setup()` 监听 `:6210`；交互式运行 `vpnagent install` 时则创建并启动服务。服务名是 `sslcon`，不是 `vpnagent`。

### 4.2 根因分级

#### A. 发布包的签名顺序有缺陷（已确认）

当前 workflow 在第 198-201 行执行 `macdeployqt` 和 `codesign AnyLink.app`，第 202-204 行才下载并复制 `vpnagent`、`sslcon`。随后第 207 行直接把 app 打包。也就是说，最终 bundle 中新增的两个可执行文件没有经过这次 app 签名；后续签名的是 installer app，不等于重新签名 payload 内的 `AnyLink.app` 及其嵌套二进制。

在启用 Hardened Runtime、Gatekeeper、launchd 或后续系统策略的 macOS 上，这可能导致 agent 无法执行或服务被 launchd 拒绝。必须对最终安装产物内的所有可执行文件重新签名并验证，不能只验证 installer 外壳。

#### B. agent 安装失败可能被静默吞掉（已确认）

外部 `sslcon/svc/service.go` 的 `InstallSvc()` 在 `svc.Install()` 或 `svc.Start()` 失败时只 `fmt.Println`，函数没有返回错误；`vpnagent.go` 也没有根据失败调用 `os.Exit(1)`。因此以下情况都可能让 Qt IFW 继续走“安装成功”路径：

- `/Library/LaunchDaemons/sslcon.plist` 已存在，service 库返回 `Init already exists`。
- 旧 plist 指向已经不存在的 `~/AnyLink/AnyLink.app/.../vpnagent`。
- `launchctl load` 因权限、签名、plist 或路径问题失败。
- 设备架构与包不匹配。

重装如果没有先清理旧 plist 或修复返回码，就可能重复失败但用户看不到真正原因。

#### C. 客户端错误处理把所有失败都归为“请重装”（已确认）

`JsonRpcWebSocketClient` 只发送一次 `open`，没有连接超时分类、退避重试、服务状态检查或 repair 流程。任何 `QWebSocket` error 都被转换成相同的提示，并禁用连接按钮。服务刚好在启动、launchd 重新加载或系统刚从睡眠恢复时，瞬时连接失败也会进入这个永久不可用状态。

错误文本中的 `vpnagent` 是二进制名称，但 launchd 中实际服务名是 `sslcon`，也没有展示 `errorString()`，用户无法据此判断是服务不存在、端口未监听、权限问题还是架构问题。

#### D. 架构限制是必须先确认的前置条件（高概率条件）

README 明确说明 v0.9.6 起 macOS 只支持 Apple Silicon；workflow 只下载 `sslcon-macOS-arm64.tar.gz`。如果目标机器是 Intel，或最终 app/agent 的架构被错误裁剪，AnyLink 和 agent 不能按当前发布策略正常运行。这个条件可以解释“某台 Mac 重装也没效果”，但必须以 `uname -m`、`file` 和 `lipo -info` 现场结果确认。

#### E. 旧服务路径/安装更新时序可能造成持续失效（需要验证）

launchd plist 的可执行路径来自安装时执行的 `vpnagent` 路径，而当前安装器把真实 app 放在用户目录，再在 `/Applications` 创建链接。若旧安装残留 plist、用户移动/删除了真实目录，或卸载没有成功执行 `Stop`/`Uninstall`，plist 可能仍指向旧路径。重新拷贝 app 不会自动修复一个已加载的旧 launchd job。

### 4.3 现场排查命令

以下命令应在受影响的 Apple Silicon Mac 上执行。它们只读系统状态；修改 launchd 前应先确认目标服务名和 plist 路径。

```bash
uname -m
file "$HOME/AnyLink/AnyLink.app/Contents/MacOS/AnyLink"
file "$HOME/AnyLink/AnyLink.app/Contents/MacOS/vpnagent"
lipo -info "$HOME/AnyLink/AnyLink.app/Contents/MacOS/vpnagent"

sudo launchctl print system/sslcon
sudo launchctl list | grep -i sslcon
sudo plutil -p /Library/LaunchDaemons/sslcon.plist
sudo lsof -nP -iTCP:6210 -sTCP:LISTEN

sudo ls -l /var/log/sslcon.out.log /var/log/sslcon.err.log
sudo tail -n 100 /var/log/sslcon.out.log /var/log/sslcon.err.log
codesign --verify --deep --strict --verbose=2 "$HOME/AnyLink/AnyLink.app"
codesign --display --verbose=4 "$HOME/AnyLink/AnyLink.app/Contents/MacOS/vpnagent"
spctl --assess --type execute --verbose=4 "$HOME/AnyLink/AnyLink.app"
```

判定重点：

- `launchctl print system/sslcon` 失败：服务没有成功注册或标签/域不对。
- plist 存在但 `ProgramArguments` 指向不存在路径：旧安装或更新清理失败。
- 没有 TCP `6210` listener：agent 没有运行、启动即退出或架构/签名失败。
- `codesign`/`spctl` 失败：发布包签名顺序或嵌套二进制签名需要修复。
- `file`/`lipo` 显示非 arm64：当前发布包与设备架构不匹配。

## 5. 综合判断

| 用户现象 | 首要怀疑 | 次要怀疑 | 需要的证据 |
| --- | --- | --- | --- |
| 关闭/最小化后 Dock 点击无窗口 | native reopen handler 返回 `false`，且 `showNormal()` 未激活窗口 | close/quit 语义混用 | Dock 回调、前台 app 状态、AnyLink 进程仍存活 |
| 重启 AnyLink 后 Dock 闪一下就消失 | 已有 primary，secondary 按设计立即退出 | primary 未响应或窗口仍 hidden | `pgrep`、SingleApplication 日志、primary 窗口状态 |
| 提示无法连接 vpnagent | `6210` 未监听或只尝试一次连接 | launchd plist 残留、安装失败被吞、agent 签名/架构错误 | `launchctl`、`lsof`、日志、codesign、file |
| 重装仍无效 | 重装没有清理/重建有效的 `sslcon.plist`，或错误未暴露 | 当前机器不在 arm64 支持范围 | 安装器日志、plist 的真实路径、安装命令退出码 |

结论是：窗口问题应先修复 AnyLink GUI 的 macOS 生命周期；服务问题应同时修复发布签名、agent 安装返回码和客户端诊断重试。单独重装 GUI 或只修改提示文字不能解决第二类问题。
