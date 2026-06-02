#!/usr/bin/env sh
# Builds and runs the tuple-layout regression package and asserts it exits 0.
# Tests/Tuple checks sizeof and runtime field offsets for mixed-alignment tuples.
# Returns 0 only when every case passes, so a non-zero exit means layout regressed.
#
# Usage:
#   Tests/run_tuple_test.sh [path-to-rux]
#   RUX=/path/to/rux Tests/run_tuple_test.sh
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

pkg="$SCRIPT_DIR/Tuple"
( cd "$pkg" && "$RUX" build >/dev/null )
bin="$pkg/Bin/Debug/tuple_test"
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
    echo "PASS: tuple layout (Tests/Tuple)"
    exit 0
fi
echo "FAIL: tuple layout (Tests/Tuple) — case $code returned the wrong value (exit $code)" >&2
exit 1
