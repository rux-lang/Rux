#!/usr/bin/env sh
# Builds and runs the bool-bitwise regression package and asserts it exits 0.
# Tests/BoolBitwise covers bitwise &, |, ^, ~, <<, >> on bool8/bool16/bool32
# (see issue #95 — the docs advertise this, but the compiler previously rejected
# every one of these with "bitwise operator applied to non-integer type").
# Returns 0 only when every case passes; non-zero exit means a case regressed.
#
# Usage:
#   Tests/run_bool_bitwise_test.sh [path-to-rux]
#   RUX=/path/to/rux Tests/run_bool_bitwise_test.sh
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

pkg="$SCRIPT_DIR/BoolBitwise"
( cd "$pkg" && "$RUX" build >/dev/null )
bin="$pkg/Bin/Debug/bool_bitwise_test"
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
    echo "PASS: bool bitwise (Tests/BoolBitwise)"
    exit 0
fi
echo "FAIL: bool bitwise (Tests/BoolBitwise) — case $code returned the wrong value (exit $code)" >&2
exit 1
