#!/usr/bin/env sh
# Builds and runs the integer ** (exponentiation) regression package and
# asserts it exits 0. Tests/Pow returns 0 only when every integer ** case
# matches its expected value, so a non-zero exit (or a failure to launch, which
# is how the missing __rux_ipow helper used to manifest) means the operator
# regressed.
#
# Usage:
#   Tests/run_pow_test.sh [path-to-rux]
#   RUX=/path/to/rux Tests/run_pow_test.sh
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

pkg="$SCRIPT_DIR/Pow"
( cd "$pkg" && "$RUX" build --target linux-x64 >/dev/null )
bin="$pkg/Bin/Debug/pow_test"
[ -f "$bin" ] || bin="$bin.exe"
if [ ! -f "$bin" ]; then
    echo "error: built executable not found at $bin" >&2
    exit 2
fi

set +e
"$bin"
code=$?
set -e
if [ "$code" -eq 0 ]; then
    echo "PASS: integer ** (Tests/Pow)"
    exit 0
fi
echo "FAIL: integer ** (Tests/Pow) — case $code returned the wrong value (exit $code)" >&2
exit 1
