#!/usr/bin/env bash

set -euo pipefail

archive=""
binary_dir=""
bundle=""
expected_sha256=""
expected_arch="arm64"

while [[ "$#" -gt 0 ]]; do
    case "$1" in
    --archive)
        archive="$2"
        shift 2
        ;;
    --binary-dir)
        binary_dir="$2"
        shift 2
        ;;
    --bundle)
        bundle="$2"
        shift 2
        ;;
    --expected-sha256)
        expected_sha256="$2"
        shift 2
        ;;
    --architecture)
        expected_arch="$2"
        shift 2
        ;;
    *)
        printf 'unknown argument: %s\n' "$1" >&2
        exit 2
        ;;
    esac
done

if [[ -z "$archive" || -z "$expected_sha256" ]]; then
    printf 'usage: %s --archive FILE --expected-sha256 SHA256 [--binary-dir DIR] [--bundle APP]\n' "$0" >&2
    exit 2
fi

if [[ ! -f "$archive" ]]; then
    printf 'agent archive is missing: %s\n' "$archive" >&2
    exit 1
fi

actual_sha256="$(shasum -a 256 "$archive" | awk '{print $1}')"
if [[ "$actual_sha256" != "$expected_sha256" ]]; then
    printf 'agent archive checksum mismatch: expected %s, got %s\n' \
        "$expected_sha256" "$actual_sha256" >&2
    exit 1
fi

if [[ -z "$binary_dir" && -z "$bundle" ]]; then
    printf 'one of --binary-dir or --bundle is required\n' >&2
    exit 2
fi

if [[ -n "$bundle" ]]; then
    binary_dir="$bundle/Contents/MacOS"
fi

for binary in vpnagent sslcon; do
    path="$binary_dir/$binary"
    if [[ ! -x "$path" ]]; then
        printf 'agent binary is missing or not executable: %s\n' "$path" >&2
        exit 1
    fi

    if ! file_output="$(file "$path")"; then
        printf 'cannot inspect agent binary: %s\n' "$path" >&2
        exit 1
    fi
    if [[ "$file_output" != *"$expected_arch"* ]]; then
        printf 'agent binary has wrong architecture: %s (%s)\n' "$path" "$file_output" >&2
        exit 1
    fi
    if ! lipo -info "$path" | grep -Fq "$expected_arch"; then
        printf 'agent binary does not contain %s: %s\n' "$expected_arch" "$path" >&2
        exit 1
    fi
    printf '%s: %s\n' "$path" "$file_output"
    lipo -info "$path"
done

if [[ -n "$bundle" ]]; then
    codesign --verify --deep --strict --verbose=2 "$bundle"
    codesign --display --verbose=4 "$bundle" 2>&1
    spctl --assess --type execute --verbose=4 "$bundle"
    printf 'launchd plist: /Library/LaunchDaemons/sslcon.plist\n'
fi

printf 'PASS: macOS agent archive and bundle audit (%s)\n' "$expected_arch"
