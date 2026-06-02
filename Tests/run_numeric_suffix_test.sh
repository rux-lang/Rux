#!/usr/bin/env sh
# Builds and runs the numeric-literal-suffix regression package and asserts it
# exits 0. Tests/NumericSuffix checks that hex/binary/octal integer literals
# accept a type suffix (0xFFu, 0b1010u, 0o17i) and '_' separators, returning the
# 1-based index of the first wrong value, so a non-zero exit means the lexer
# regressed on suffixed non-decimal literals.
#
# Usage:
#   Tests/run_numeric_suffix_test.sh [path-to-rux]
#   RUX=/path/to/rux Tests/run_numeric_suffix_test.sh
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

pkg="$SCRIPT_DIR/NumericSuffix"
( cd "$pkg" && "$RUX" build >/dev/null )
bin="$pkg/Bin/Debug/numeric_suffix_test"
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
    echo "PASS: numeric literal suffixes (Tests/NumericSuffix)"
    exit 0
fi
echo "FAIL: numeric literal suffixes (Tests/NumericSuffix) — check $code returned the wrong value (exit $code)" >&2
exit 1
