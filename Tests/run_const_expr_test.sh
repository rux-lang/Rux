#!/usr/bin/env sh
# Builds and runs the constant-expression coercion regression package and
# asserts it exits 0. Tests/ConstExpr assigns constant integer expressions
# (e.g. 10 + 2 * (5 - 3)) to sized integer types and checks the values, so a
# non-zero exit means literal-expression coercion regressed. A failure to build
# would mean the expressions were rejected outright, which is the bug this fix
# addresses.
#
# Usage:
#   Tests/run_const_expr_test.sh [path-to-rux]
#   RUX=/path/to/rux Tests/run_const_expr_test.sh
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

pkg="$SCRIPT_DIR/ConstExpr"
( cd "$pkg" && "$RUX" build >/dev/null )
bin="$pkg/Bin/Debug/const_expr_test"
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
    echo "PASS: constant expression coercion (Tests/ConstExpr)"
    exit 0
fi
echo "FAIL: constant expression coercion (Tests/ConstExpr) — check $code returned the wrong value (exit $code)" >&2
exit 1
