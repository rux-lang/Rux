#!/usr/bin/env sh
# Regression test for issue #64.
# Builds Tests/TargetImport and asserts the build succeeds (exit 0).
#
# The package contains a dependency (PlatformLib) that has both
# @[Target("Linux")] and @[Target("Windows")] imports. Before the fix,
# the compiler collected all imports regardless of @[Target], causing:
#   error: package 'LinuxHelper' is not listed in [Dependencies]
# on Windows (and the same for WinHelper on Linux).
#
# A successful build — no error, exit 0 — is the assertion.
#
# Usage:
#   Tests/run_target_import_test.sh [path-to-rux]
#   RUX=/path/to/rux Tests/run_target_import_test.sh
#
# The rux binary is taken from $RUX, then the first argument, then a few common
# build locations, then $PATH.
set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

RUX="${RUX:-${1:-}}"
if [ -z "$RUX" ]; then
    for candidate in \
        "$SCRIPT_DIR/../build/rux" \
        "$SCRIPT_DIR/../build/clang/rux" \
        "$SCRIPT_DIR/../build/msvc/rux" \
        "$SCRIPT_DIR/../build/Release/rux" \
        "$(command -v rux 2>/dev/null || true)"; do
        if [ -n "$candidate" ] && [ -x "$candidate" ]; then
            RUX="$candidate"
            break
        fi
    done
fi
if [ -z "$RUX" ]; then
    echo "error: rux binary not found; set RUX or pass it as the first argument" >&2
    exit 2
fi
echo "Using rux: $RUX"

pkg="$SCRIPT_DIR/TargetImport"
set +e
( cd "$pkg" && "$RUX" build 2>&1 )
code=$?
set -e

if [ "$code" -eq 0 ]; then
    echo "PASS: @[Target] import filtering (Tests/TargetImport)"
    exit 0
fi
echo "FAIL: @[Target] import filtering (Tests/TargetImport) — build exited $code" >&2
exit 1
