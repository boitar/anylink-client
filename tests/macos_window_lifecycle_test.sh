#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
anylink_cpp="$repo_root/src/anylink.cpp"
anylink_h="$repo_root/src/anylink.h"
main_cpp="$repo_root/src/main.cpp"
dock_mm="$repo_root/src/macdockiconhandler.mm"
rpc_h="$repo_root/src/jsonrpcwebsocketclient.h"
rpc_cpp="$repo_root/src/jsonrpcwebsocketclient.cpp"
anylink_ui="$repo_root/src/anylink.ui"
release_yml="$repo_root/.github/workflows/release.yml"
installer_js="$repo_root/installer/packages/root/meta/component.js"
single_application_cpp="$repo_root/3rdparty/SingleApplication/singleapplication.cpp"

assert_contains() {
    local file="$1"
    local text="$2"
    local label="$3"

    if ! grep -Fq "$text" "$file"; then
        printf 'FAIL: %s\n' "$label" >&2
        printf '  missing: %s\n' "$text" >&2
        exit 1
    fi
}

assert_not_contains() {
    local file="$1"
    local text="$2"
    local label="$3"

    if grep -Fq "$text" "$file"; then
        printf 'FAIL: %s\n' "$label" >&2
        printf '  unexpected: %s\n' "$text" >&2
        exit 1
    fi
}

assert_contains "$anylink_h" "void showMainWindow();" "showMainWindow is a public slot"
assert_contains "$anylink_h" "bool m_initialized = false;" "initialization has an explicit state"
assert_contains "$anylink_h" "bool m_initializationScheduled = false;" "initialization scheduling is guarded"
assert_contains "$anylink_h" "bool m_quitRequested = false;" "quit has an explicit state"

assert_contains "$anylink_cpp" "    show();" "window restoration shows a hidden window"
assert_contains "$anylink_cpp" "    showNormal();" "window restoration clears minimized state"
assert_contains "$anylink_cpp" "    raise();" "window restoration raises the window"
assert_contains "$anylink_cpp" "    activateWindow();" "window restoration activates the window"
assert_contains "$anylink_cpp" "    ForceActivation();" "macOS restoration activates the application"
assert_contains "$anylink_cpp" "ensureInitialized();" "window restoration initializes before showing"
assert_contains "$anylink_cpp" "if (!m_initialized && !m_initializationScheduled)" "show events cannot schedule duplicate initialization"

show_normal_count="$(grep -Fc 'showNormal();' "$anylink_cpp")"
if [[ "$show_normal_count" != "1" ]]; then
    printf 'FAIL: showNormal must only be used by showMainWindow (found %s uses)\n' "$show_normal_count" >&2
    exit 1
fi

close_call_count="$(grep -Ec '^[[:space:]]*close\(\);$' "$anylink_cpp" || true)"
if [[ "$close_call_count" != "0" ]]; then
    printf 'FAIL: business paths must not use close() as a hide/quit switch\n' >&2
    exit 1
fi

assert_contains "$anylink_cpp" "connect(actionConfig, &QAction::triggered, this, &AnyLink::showMainWindow);" "Dock menu Show Panel uses the unified restoration path"
assert_contains "$anylink_cpp" "connect(actionQuit, &QAction::triggered, this, &AnyLink::quitApplication);" "Dock menu Quit uses the explicit quit path"
assert_contains "$anylink_cpp" "connect(dockIconHandler, &MacDockIconHandler::dockIconClicked, this, &AnyLink::showMainWindow);" "Dock reopen uses the unified restoration path"
assert_contains "$anylink_cpp" "hideMainWindow();" "automatic minimize uses the hide path"
assert_contains "$anylink_cpp" "m_quitRequested = true;" "explicit quit marks the application as quitting"
assert_contains "$anylink_cpp" "timer.stop();" "explicit quit stops periodic work"
assert_contains "$anylink_cpp" "QTimer::singleShot(1000, this" "explicit quit has a bounded disconnect wait"

assert_contains "$main_cpp" "QApplication::setQuitOnLastWindowClosed(false);" "application lifetime is configured before the window is shown"
assert_contains "$main_cpp" "&AnyLink::showMainWindow" "secondary launch wakes the primary window"
assert_contains "$main_cpp" "w.showMainWindow();" "cold start uses the same restoration path"

quit_line="$(grep -nF 'QApplication::setQuitOnLastWindowClosed(false);' "$main_cpp" | cut -d: -f1)"
show_line="$(grep -nF 'w.showMainWindow();' "$main_cpp" | cut -d: -f1)"
if (( quit_line >= show_line )); then
    printf 'FAIL: quit-on-last-window configuration must precede the first show\n' >&2
    exit 1
fi

assert_contains "$dock_mm" "static BOOL dockClickHandler(id self, SEL _cmd, NSApplication *application, BOOL hasVisibleWindows)" "AppKit reopen callback has the complete selector signature"
assert_contains "$dock_mm" 'class_replaceMethod(delClass, shouldHandle, (IMP)dockClickHandler, "B@:@B");' "AppKit method encoding includes both selector arguments"
assert_contains "$dock_mm" "return YES;" "Dock reopen does not suppress AppKit default behavior"
assert_not_contains "$dock_mm" "return false;" "Dock reopen is not unconditionally blocked"

assert_contains "$single_application_cpp" "::kill(static_cast<pid_t>(inst->primaryPid), 0)" "stale primary PID is checked before secondary exit"
assert_contains "$single_application_cpp" "if (!primaryAlive)" "stale SingleApplication state can be recovered"

assert_contains "$rpc_h" "enum class ConnectionState" "agent connection has explicit states"
assert_contains "$rpc_h" "enum class FailureCategory" "agent failures have explicit categories"
assert_contains "$rpc_h" "void retryNow();" "agent connection exposes manual retry"
assert_contains "$rpc_h" "void connectionError(" "agent connection exposes raw errors"
assert_contains "$rpc_h" "static constexpr int ConnectionTimeoutMs = 3000;" "agent connection has a timeout"
assert_contains "$rpc_cpp" "return 500 * (1 << exponent);" "agent connection uses exponential backoff"
assert_contains "$rpc_h" "MaxRetryCount = 5" "agent retry count is bounded"
assert_not_contains "$anylink_cpp" "please reinstall the software" "agent errors are not reduced to reinstall advice"
assert_contains "$anylink_cpp" "Retry Local Agent Connection" "agent retry is available from the tray menu"
assert_contains "$anylink_cpp" "Agent diagnostics copied to the clipboard." "agent diagnostics can be copied"
assert_contains "$anylink_ui" "name=\"buttonAgentCheck\"" "agent retry is available in the UI"
assert_contains "$anylink_ui" "name=\"buttonCopyDiagnostics\"" "agent diagnostics are available in the UI"

assert_contains "$release_yml" "SSLCON_MACOS_ASSET_ID: '455331558'" "macOS agent asset is pinned"
assert_contains "$release_yml" "SSLCON_MACOS_ARM64_SHA256:" "macOS agent checksum is pinned"
assert_contains "$release_yml" "verify-macos-agent.sh" "release verifies the final macOS agent bundle"
assert_not_contains "$release_yml" "continue-on-error: \${{ matrix.build == 'macos-arm64' }}" "macOS release failures are not ignored"
assert_contains "$installer_js" "macos-agent-repair.sh" "installer removes stale launchd state"
assert_contains "$installer_js" "macos-agent-health-check.sh" "installer checks launchd and port health"
assert_contains "$installer_js" 'installer.setValue("RemoveTargetDir", "false")' "offline macOS installer can replace a stale maintenance tool"
assert_contains "$installer_js" 'systemInfo.kernelType !== "darwin"' "macOS installer does not launch the previous maintenance tool"
assert_contains "$repo_root/installer/config/controller.js" 'installer.setValue("RemoveTargetDir", "false")' "target page keeps repair mode after navigating back"
assert_contains "$repo_root/installer/config/controller.js" 'installer.setValue("RemoveTargetDir", "true")' "macOS installation restores target cleanup before installation"

printf 'PASS: macOS window lifecycle regression checks\n'
