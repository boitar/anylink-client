# macOS 改造方案与任务拆解

本文档按照两个用户问题拆分排查和改造任务。现场处理时先根据用户现象选择对应章节，不要默认两个问题由同一个原因导致。

| 问题 | 业务现象 | 排查主线 | 任务前缀 |
| --- | --- | --- | --- |
| 问题一 | 窗口关闭/最小化后无法呼出，重启应用只在 Dock 闪一下 | AnyLink 进程、Dock reopen、SingleApplication、窗口激活 | `W-*` |
| 问题二 | 提示无法连接 `vpnagent`，重装后仍无效 | `sslcon` launchd 服务、6210 端口、安装器、签名、架构、连接重试 | `A-*` |

两个问题的共同原则：保留真实错误和现场证据；修复后必须验证最终用户安装包，而不是只验证源码能够编译。

## 问题一：窗口关闭或最小化后无法呼出

### 1.1 业务现象

用户可能遇到以下任一表现：

- AnyLink 第一次启动窗口正常。
- 关闭窗口或点击最小化后，点击 Dock 图标没有窗口。
- VPN 连接后启用了“连接时最小化”，之后无法恢复面板。
- 再次打开 AnyLink 时，Dock 图标出现一下就消失，窗口仍不出现。

这条排查线只关注 GUI 进程和窗口生命周期。即使 `vpnagent` 同时不可用，也不要把窗口无法显示直接归因于服务问题。

### 1.2 当前实现和根因

当前窗口路径为：

```text
用户关闭/最小化窗口
  -> AnyLink::closeEvent()
  -> 已连接时 hide()，未连接时 qApp->quit()
  -> 进程可能继续存活，但窗口不可见

Dock 点击
  -> applicationShouldHandleReopen:hasVisibleWindows:
  -> macOS native handler 发出 Qt 信号并返回 false
  -> AnyLink 只调用 showNormal()
  -> 没有统一 show/raise/activateWindow

再次启动 AnyLink
  -> SingleApplication 发现已有 primary
  -> 新进程作为 secondary 通知 primary 后立即退出
  -> primary 未可靠置前时，用户只看到 Dock 闪一下
```

已确认的代码问题：

1. `src/macdockiconhandler.mm` 使用 `class_replaceMethod` 替换带参数的 AppKit selector，但类型编码为 `B@:`，方法签名不完整，属于未定义行为风险。
2. Dock reopen handler 固定返回 `false`，主动禁止 macOS 默认 reopen 行为。
3. Dock 点击、Dock 菜单 `Show Panel` 和 `SingleApplication::instanceStarted` 最终都只调用 `showNormal()`，没有统一执行 `show()`、`raise()`、`activateWindow()` 和 macOS 应用激活。
4. `closeEvent()` 同时承担隐藏和退出，`actionQuit` 又通过 `close()` 间接触发它；窗口关闭、连接后自动隐藏和用户主动退出没有显式区分。
5. `afterShowOneTime()` 通过 `trayIcon == nullptr` 判断初始化状态，窗口恢复依赖一次延迟的 `showEvent()` 初始化，生命周期边界不够明确。

### 1.3 业务排查顺序

在受影响的 Mac 上按以下顺序确认：

1. 窗口消失后检查 AnyLink 进程是否仍存在。如果进程存在，优先排查 Dock reopen 和窗口激活；如果进程不存在，排查退出路径和崩溃日志。
2. 窗口消失后通过 Dock 点击一次，再启动一次 AnyLink。若第二次启动只短暂出现，说明新进程大概率是 secondary，重点查看 primary 是否仍存活以及是否收到唤醒事件。
3. 分别测试 Dock 图标、Dock 菜单 `Show Panel` 和重复启动。如果三条入口都失败，优先修复统一窗口激活入口；如果只有 Dock 点击失败，优先修复 native reopen callback。
4. 分别测试 VPN 已连接和未连接状态，确认关闭窗口不会意外退出进程或断开 VPN。

建议记录：AnyLink PID、窗口 `visible/hidden/minimized` 状态、macOS 前台应用状态、触发入口、`SingleApplication` 日志和崩溃日志。

### 1.4 解决方案

#### 统一窗口恢复入口

在 `AnyLink` 中增加幂等的 `showMainWindow()`，所有恢复入口都调用它：

```text
showMainWindow()
  -> ensureInitialized()
  -> show()
  -> showNormal()
  -> raise()
  -> activateWindow()
  -> macOS: activateIgnoringOtherApps / 等价 Qt 激活
```

Dock reopen callback、Dock 菜单 `Show Panel` 和 `instanceStarted` 不再各自实现显示逻辑。验收标准是窗口在前台可见、未最小化且可交互，而不是只验证 `showNormal()` 被调用。

#### 分离隐藏和退出语义

建议使用显式的 `quitRequested` 状态：

- 普通关闭：隐藏窗口，保留 GUI 进程和 VPN 状态。
- Dock 菜单 `Quit`：设置 `quitRequested`，停止定时器，按有界超时断开 VPN，然后退出应用。
- 连接后的自动最小化：只调用隐藏逻辑，不能走退出逻辑。
- `setQuitOnLastWindowClosed(false)` 在主窗口显示前设置，作为进程存活兜底，不作为窗口恢复机制。

如果产品必须保持“未连接时关闭即退出”，也必须通过显式退出标记区分用户关闭和系统/业务触发的隐藏，不能继续只用 `m_vpnConnected` 猜测意图。

#### 修复 macOS native reopen

- 使用与 `applicationShouldHandleReopen:hasVisibleWindows:` 完全匹配的 Objective-C 方法签名。
- 不要无条件返回 `false` 阻断系统行为；如果需要自定义行为，必须在 Qt 激活逻辑完成后返回与设计一致的结果。
- 将 `ForceActivation` 纳入统一恢复路径，或删除未使用实现，避免存在两套激活机制。
- 将 `afterShowOneTime()` 改为显式 `m_initialized` guard，确保菜单、RPC 和状态连接只初始化一次且可观测。

### 1.5 问题一任务拆解

| ID | 任务 | 主要范围 | 依赖 | 完成标准 |
| --- | --- | --- | --- | --- |
| W-0 | 采集窗口故障基线 | macOS、AnyLink 进程、Dock、崩溃日志 | 无 | 分别记录关闭、最小化、自动隐藏和重复启动四种场景 |
| W-1 | 增加统一 `showMainWindow()` | `src/anylink.{h,cpp}` | W-0 | 统一执行 show、showNormal、raise、activateWindow 和 macOS 激活 |
| W-2 | 修正 AppKit reopen callback | `src/macdockiconhandler.{h,mm}` | W-1 | selector 签名正确；Dock 点击能恢复窗口；不再无条件阻断默认 reopen |
| W-3 | 接通 secondary 唤醒 | `src/main.cpp` | W-1 | 再次启动只唤醒 primary；primary 窗口前台可见；secondary 正常退出 |
| W-4 | 分离 close/hide/quit | `src/anylink.{h,cpp}` | W-1 | 普通关闭、自动最小化、Dock Quit 的行为互不误触发 |
| W-5 | 显式化一次性初始化 | `src/anylink.{h,cpp}` | W-1 | 不再用 `trayIcon == nullptr` 作为唯一初始化状态；重复显示不会重复连接信号 |
| W-6 | 建立窗口回归测试 | Qt Test 或 macOS 自动化脚本 | W-2、W-3、W-4 | 覆盖 Dock、Dock 菜单、重复启动、最小化、隐藏和显式退出 |

### 1.6 问题一验收矩阵

| 场景 | 预期结果 |
| --- | --- |
| 冷启动 | 主窗口可见、前台、可交互 |
| VPN 已连接后点击关闭 | 窗口隐藏，进程和 VPN 保持；再次点 Dock 可恢复 |
| VPN 已连接后点击最小化 | Dock 点击和 `Show Panel` 都能恢复并置前 |
| 连接时自动最小化 | 不退出 GUI；用户仍能通过 Dock 或再次启动恢复 |
| 窗口隐藏后再次启动 | secondary 可以退出，但 primary 必须在前台显示窗口 |
| Dock 菜单 `Show Panel` | hidden/minimized/后台状态都能恢复 |
| Dock 菜单 `Quit` | 停止连接、停止定时器并退出；不留下意外后台 GUI |
| 快速重复启动多个实例 | 只有一个 primary；每个 secondary 都能唤醒同一窗口 |

## 问题二：无法连接 vpnagent，重装后仍无效

### 2.1 业务现象

用户看到“未能连接到 vpnagent 服务，请重新安装本软件”，但重装后仍然无法连接。该提示目前不能区分以下情况：

- 服务没有安装或没有启动。
- launchd plist 已存在但指向旧 app 路径。
- `vpnagent` 启动失败或端口 `6210` 被其他程序占用。
- agent 架构与 Mac 不匹配。
- agent/嵌套二进制签名无效，被 macOS 拒绝执行。
- 服务刚启动，GUI 的第一次连接发生得太早。
- WebSocket/JSON-RPC 协议或服务内部发生错误。

这条排查线只关注 agent 安装、launchd、端口和 GUI-RPC 连接。窗口是否能显示应按问题一单独判断。

### 2.2 当前实现和根因

当前服务链路为：

```text
安装器
  -> 真实 app: ~/AnyLink/AnyLink.app
  -> /Applications/AnyLink.app 创建链接
  -> 提权执行 .../Contents/MacOS/vpnagent install
  -> 外部 service 库创建 /Library/LaunchDaemons/sslcon.plist
  -> launchctl load
  -> sslcon 服务监听 127.0.0.1:6210

AnyLink GUI
  -> 一次性连接 ws://127.0.0.1:6210/rpc
  -> 发送 config/connect/status/reconnect
```

已确认或需要重点验证的原因：

1. GUI 只尝试一次 WebSocket 连接；任何错误都显示“请重装”、禁用连接按钮，不保存真实 `errorString()`，瞬时启动时序也会变成永久不可用。
2. 外部 `sslcon` 服务的 launchd 标签是 `sslcon`，不是 `vpnagent`；如果现场只检查 `vpnagent` 服务名，可能得到错误结论。
3. 外部 `InstallSvc()`/`UninstallSvc()` 在失败时只打印错误，没有可靠返回非零退出码，Qt Installer Framework 可能继续按安装成功处理。
4. 当前 macOS workflow 先对 `AnyLink.app` 签名，之后才把 `vpnagent` 和 `sslcon` 拷入 app bundle；最终嵌套二进制可能没有被签名，launchd 可能因此无法执行 agent。
5. v0.9.6 的 macOS 发布只支持 Apple Silicon，workflow 下载的是 `sslcon-macOS-arm64.tar.gz`。Intel Mac 或错误架构的 agent 无法工作。
6. 旧的 `/Library/LaunchDaemons/sslcon.plist` 可能仍指向已经删除或更新前的 `~/AnyLink/AnyLink.app`；重新覆盖 app 不会自动修复已经加载的旧 job。
7. agent 从外部 `latest` release 下载，依赖没有固定版本和 SHA-256，导致同一个 GUI 构建可能装入不同服务二进制，难以复现问题。

### 2.3 业务排查顺序

在受影响的 Apple Silicon Mac 上，先执行只读检查：

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

按结果分流：

| 检查结果 | 业务判断 | 下一步 |
| --- | --- | --- |
| `uname -m` 不是 `arm64` | 当前 v0.9.6 不在支持范围 | 先明确支持策略，不要反复重装同一版本 |
| `launchctl print system/sslcon` 失败，且无 plist | 服务未安装 | 检查安装器提权操作和安装命令退出码 |
| plist 存在但 `ProgramArguments` 指向旧路径 | 服务残留/升级清理失败 | 执行 repair/upgrade 逻辑，先卸载旧 job 再注册新路径 |
| plist 存在但没有 PID | 服务已注册但启动失败 | 查看 `sslcon.err.log`、签名、架构和可执行权限 |
| 6210 没有 listener | agent 未运行或启动后退出 | 结合 launchd 状态和 agent 日志继续分流 |
| 6210 被其他进程占用 | 端口冲突 | 记录占用进程，修复服务端口配置或提示用户 |
| `codesign`/`spctl` 失败 | 发布包或安装后的嵌套二进制签名有问题 | 修复打包签名顺序并重新发布 |
| 6210 已监听但 GUI 仍报错 | GUI 连接时序、WebSocket 或协议问题 | 查看 Qt socket 原始错误并增加重试/协议诊断 |

### 2.4 解决方案

#### GUI 连接状态和诊断

在 `JsonRpcWebSocketClient` 或新的 `AgentConnectionManager` 中明确状态：

```text
Unknown -> Connecting -> Ready
                   \-> Retrying -> Ready
                   \-> Unavailable / ProtocolError
```

具体要求：

- 保存 `QAbstractSocket::SocketError`、`errorString()`、最后连接时间和重试次数。
- 首次失败按 0.5 s、1 s、2 s、4 s、8 s 退避重试，并设置总时长上限。
- 服务未就绪时显示“正在等待本地服务”，不要立即永久禁用连接按钮。
- 区分未安装、未运行、端口冲突、签名/架构错误、暂时不可达和协议错误。
- 提供重新检测、查看 agent 日志和复制诊断信息的入口。
- 连接成功后取消重试；服务断开后进入可恢复状态，而不是只显示“请重装”。

#### 固定 agent 依赖和验证最终包

1. 将 workflow 中的 `sslcon` `latest` 改为固定版本或不可变 commit 对应资产。
2. 下载后校验 SHA-256，并验证 `vpnagent`/`sslcon` 存在、可执行且为 arm64。
3. 先把 agent/CLI 放入 `AnyLink.app/Contents/MacOS`，再按由内到外顺序签名 nested frameworks、`vpnagent`、`sslcon` 和 app。
4. 用 `codesign --verify --deep --strict`、`spctl`、`file`/`lipo` 检查最终 app，而不是只检查 installer 外壳。
5. 将已经验证的 app 归档进 IFW package，再签名和公证 installer。
6. 构建失败、公证失败、签名失败或 hash 不匹配都必须阻断 release，不能由 macOS job 的 `continue-on-error` 吞掉。

#### 让 launchd 安装可重复、可验证

- 修复外部 `vpnagent install`/`uninstall`，让失败返回非零退出码；AnyLink 应锁定包含该修复的 sslcon 版本。
- 安装/升级前检查旧 `sslcon.plist`，有序执行 `bootout/unload`、删除旧 plist、写入新 plist、`bootstrap/load`。
- 记录真实 agent 路径、服务标签、架构、签名结果和日志路径。
- 明确设置 `RunAtLoad`/`KeepAlive`，保证系统重启后 agent 可自动启动。
- 安装成功前验证 `launchctl print system/sslcon` 和 `127.0.0.1:6210` listener；失败时终止安装并显示日志位置。
- 评估把 agent 放到稳定的受保护目录；如果继续放在 `~/AnyLink`，更新、卸载和移动 app 时必须同步维护 plist。

### 2.5 问题二任务拆解

| ID | 任务 | 主要范围 | 依赖 | 完成标准 |
| --- | --- | --- | --- | --- |
| A-0 | 采集服务故障基线 | `launchctl`、plist、6210、日志、签名、架构 | 无 | 明确是未安装、未运行、端口冲突、签名/架构还是 GUI 连接问题 |
| A-1 | 增加 GUI agent 状态机和原始错误 | `src/jsonrpcwebsocketclient.*`、`src/anylink.cpp` | A-0 | UI 能显示实际错误类别和 socket 原文，不再统一提示重装 |
| A-2 | 增加连接退避和健康检查 | `src/jsonrpcwebsocketclient.*`、UI | A-1 | agent 延迟启动时自动恢复；服务不可用时可重新检测 |
| A-3 | 固定 sslcon 版本和 SHA-256 | `.github/workflows/release.yml`、release manifest | A-0 | 同一 GUI commit 取得同一 agent；hash/架构不符时 workflow 失败 |
| A-4 | 调整最终 app 的签名顺序 | `.github/workflows/release.yml` | A-3 | agent 拷入后再签名；app、agent、CLI 和 nested frameworks 验证通过 |
| A-5 | 修复 agent 安装命令退出码 | 外部 `tlslink/sslcon`，并锁定修复版 release | A-3 | plist 已存在、权限失败、启动失败时返回非零并保留日志 |
| A-6 | 让安装/升级/卸载幂等 | `installer/packages/root/meta/component.js`、agent service 管理 | A-5 | 不残留旧 job、旧 plist、旧路径或旧进程 |
| A-7 | 增加安装后健康检查 | `installer/config/controller.js` 或 installer operation | A-5、A-6 | 安装成功前确认服务状态和 6210 listener |
| A-8 | 建立 agent/service 回归矩阵 | installer + macOS 实体机/VM | A-2、A-4、A-7 | 覆盖 fresh install、reboot、repair、upgrade、uninstall、端口冲突和启动延迟 |
| A-9 | 增加最终产物审计 | `.github/workflows` 或 `scripts/` | A-3、A-4 | 自动输出架构、签名身份、Team ID、entitlements、plist 路径和 sha256 |

### 2.6 问题二验收矩阵

| 场景 | 预期结果 |
| --- | --- |
| fresh install | installer 成功后 `launchctl print system/sslcon` 成功且 6210 监听 |
| 重启 macOS | `sslcon` agent 自动启动，GUI 首次连接可等待并重试 |
| agent 延迟启动 | UI 显示 retrying，服务起来后自动变为 Ready |
| 服务未安装 | UI 明确显示未安装或未注册，并给出诊断入口 |
| plist 指向旧路径 | repair/upgrade 清理并重建，不能静默成功 |
| agent 二进制缺失 | 安装/健康检查失败，并给出真实路径和日志 |
| agent 未签名或签名无效 | release pipeline 或安装后检查失败，错误明确 |
| Intel Mac | 安装/启动前明确提示当前版本只支持 Apple Silicon |
| 6210 被其他进程占用 | UI 和 agent 日志显示端口冲突，不提示泛化重装 |
| 6210 已监听但 RPC 异常 | 显示 WebSocket/协议错误原文，支持重新检测 |
| uninstall | launchd job、plist、真实 app 和 `/Applications` 链接按预期清理 |
| upgrade from v0.9.6 | 旧 agent 停止、旧 plist 清理、新 agent 注册，升级后能连接 |

## 两个问题的实施边界

### 先做哪条线

1. 如果用户说“Dock 点击和再次启动都没有窗口”，先做 `W-0` 到 `W-5`，不需要等待 agent 问题完全解决；窗口恢复测试可以使用一个模拟的本地 RPC 服务。
2. 如果用户说“窗口能打开，但状态栏提示无法连接 vpnagent”，先做 `A-0`，按 `launchctl`、6210、签名和架构结果分流；不要先重复安装。
3. 如果两个现象同时存在，先确认 AnyLink 进程是否还在，再分别执行两条线。GUI 无法显示不等于 agent 不存在，agent 不可用也不应阻止窗口恢复。

### 共享但不混淆的发布要求

窗口修复和 agent 修复可以独立开发、独立回归，但最终发布需要同时满足：

- macOS app bundle 中 GUI、Qt 依赖、`vpnagent` 和 `sslcon` 的架构、签名和公证均有效。
- Dock reopen、Dock 菜单和 SingleApplication secondary 三条窗口恢复路径都能把 primary 窗口置前。
- agent 安装失败通过非零退出码传递到 installer，安装后能验证 `sslcon` 和 6210。
- fresh install、reboot、repair、upgrade、uninstall 和两条问题的回归日志都被保留。

## 总体 Definition of Done

本轮 macOS 改造完成的判定必须按两条问题分别通过：

### 问题一通过条件

- hidden/minimized/后台状态下，Dock 点击、Dock 菜单和再次启动都能恢复同一个 primary 窗口。
- AppKit callback 使用正确签名，不再无条件返回 `false` 阻断恢复。
- close、hide、auto-minimize、quit 语义显式分离。

### 问题二通过条件

- GUI 能显示真实 agent 连接状态和错误，并支持有限退避重试。
- agent 安装/升级/卸载失败可被 installer 感知，launchd 状态和 6210 listener 有安装后健康检查。
- agent 依赖固定版本并校验 hash；拷入 bundle 后完成签名、架构校验、公证和最终产物审计。

只完成其中一条线，不能宣称 macOS 改造整体完成。
