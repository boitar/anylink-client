#!/bin/sh

set -eu

app_path="${1:?app bundle path is required}"
plist="/Library/LaunchDaemons/sslcon.plist"
agent_path="$app_path/Contents/MacOS/vpnagent"
cli_path="$app_path/Contents/MacOS/sslcon"

if [ "$(/usr/bin/uname -m)" != "arm64" ]; then
    echo "AnyLink macOS release requires Apple Silicon (arm64)." >&2
    exit 20
fi

if [ ! -x "$agent_path" ] || [ ! -x "$cli_path" ]; then
    echo "vpnagent or sslcon is missing from $app_path/Contents/MacOS." >&2
    exit 21
fi

/usr/bin/codesign --verify --deep --strict "$app_path"

if [ ! -f "$plist" ]; then
    echo "launchd plist is missing: $plist" >&2
    exit 22
fi

# sslcon currently uses launchctl load internally. On newer macOS versions that
# can leave the plist on disk without bootstrapping it into the system domain.
# Bootstrap explicitly and then wait for launchd to publish the service.
if ! /bin/launchctl print system/sslcon >/dev/null 2>&1; then
    /bin/launchctl enable system/sslcon >/dev/null 2>&1 || true
    /bin/launchctl bootstrap system "$plist" >/dev/null 2>&1 || true
    /bin/launchctl kickstart -k system/sslcon >/dev/null 2>&1 || true
fi

attempt=0
while ! /bin/launchctl print system/sslcon >/dev/null 2>&1; do
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 15 ]; then
        echo "sslcon is not registered in the system launchd domain." >&2
        /bin/launchctl print system/sslcon >&2 || true
        /usr/bin/plutil -p "$plist" >&2 || true
        exit 23
    fi
    /bin/sleep 1
done

attempt=0
while ! /usr/bin/nc -z -w 1 127.0.0.1 6210 >/dev/null 2>&1; do
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 15 ]; then
        echo "sslcon is registered but not listening on 127.0.0.1:6210." >&2
        /bin/launchctl print system/sslcon >&2 || true
        exit 24
    fi
    /bin/sleep 1
done
