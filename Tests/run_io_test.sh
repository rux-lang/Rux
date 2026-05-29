#!/usr/bin/env sh
# Builds and runs the I/O thunk test packages and asserts their behavior,
# verifying the linker's OS-interaction thunks end-to-end:
#   - Tests/Io   : GetStdHandle + WriteFile (stdout)
#   - Tests/Echo : GetStdHandle + ReadFile + WriteFile (stdin round-trip)
#
# Usage:
#   Tests/run_io_test.sh [path-to-rux]
#   RUX=/path/to/rux Tests/run_io_test.sh
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

# Builds the package in $1 and prints the path to its executable on stdout.
build_pkg() {
    pkg="$SCRIPT_DIR/$1"
    name="$2"
    ( cd "$pkg" && "$RUX" build >/dev/null )
    bin="$pkg/Bin/Debug/$name"
    [ -f "$bin" ] || bin="$bin.exe"
    if [ ! -f "$bin" ]; then
        echo "error: built executable not found at $bin" >&2
        exit 2
    fi
    printf '%s' "$bin"
}

failures=0

# 1. stdout test: WriteFile + GetStdHandle.
IO_BIN=$(build_pkg "Io" "io_test")
IO_EXPECTED="Hello from a Rux binary via I/O thunks!"
set +e
IO_ACTUAL=$("$IO_BIN")
IO_CODE=$?
set -e
if [ "$IO_ACTUAL" = "$IO_EXPECTED" ] && [ "$IO_CODE" -eq 0 ]; then
    echo "PASS: stdout thunks (Tests/Io)"
else
    echo "FAIL: stdout thunks (Tests/Io)" >&2
    echo "  expected: [$IO_EXPECTED] (exit 0)" >&2
    echo "  actual:   [$IO_ACTUAL] (exit $IO_CODE)" >&2
    failures=$((failures + 1))
fi

# 2. stdin round-trip: ReadFile + WriteFile + GetStdHandle.
ECHO_BIN=$(build_pkg "Echo" "echo_test")
ECHO_INPUT="round-trip via stdin thunks"
set +e
ECHO_ACTUAL=$(printf '%s' "$ECHO_INPUT" | "$ECHO_BIN")
ECHO_CODE=$?
set -e
if [ "$ECHO_ACTUAL" = "$ECHO_INPUT" ] && [ "$ECHO_CODE" -eq 0 ]; then
    echo "PASS: stdin round-trip (Tests/Echo)"
else
    echo "FAIL: stdin round-trip (Tests/Echo)" >&2
    echo "  sent:     [$ECHO_INPUT]" >&2
    echo "  received: [$ECHO_ACTUAL] (exit $ECHO_CODE)" >&2
    failures=$((failures + 1))
fi

if [ "$failures" -eq 0 ]; then
    echo "All I/O thunk tests passed"
    exit 0
fi
echo "$failures I/O thunk test(s) failed" >&2
exit 1
