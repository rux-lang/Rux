#!/usr/bin/env sh
# run_regalloc_tests.sh — exercises the linear scan register allocator
#
# Builds all four RegAlloc sub-packages and runs them; any non-zero exit code
# or build failure is treated as a test failure.
#
# Usage:
#   Tests/run_regalloc_tests.sh [path-to-rux]
#   RUX=/path/to/rux Tests/run_regalloc_tests.sh

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

PASS=0
FAIL=0

run_test() {
    pkg_rel="$1"   # e.g. RegAlloc/Loops
    exe_name="$2"  # name without extension
    pkg_dir="$SCRIPT_DIR/$pkg_rel"

    printf 'Building %-40s ... ' "$pkg_rel"
    if ! ( cd "$pkg_dir" && "$RUX" build --target linux-x64 >/dev/null 2>&1 ); then
        echo "FAIL (build error)"
        FAIL=$((FAIL+1))
        return
    fi

    bin=""
    for candidate in "$pkg_dir/Bin/Debug/$exe_name" "$pkg_dir/Bin/Debug/$exe_name.exe"; do
        if [ -f "$candidate" ] && [ -x "$candidate" ]; then
            bin="$candidate"
            break
        fi
    done

    if [ -z "$bin" ]; then
        echo "FAIL (binary not found)"
        FAIL=$((FAIL+1))
        return
    fi

    if "$bin" >/dev/null 2>&1; then
        echo "PASS"
        PASS=$((PASS+1))
    else
        ec=$?
        echo "FAIL (exit code $ec)"
        FAIL=$((FAIL+1))
    fi
}

run_test "RegAlloc/Loops"          "regalloc_loops"
run_test "RegAlloc/ManyVars"       "regalloc_manyvars"
run_test "RegAlloc/LiveAcrossCall" "regalloc_live_across_call"
run_test "RegAlloc/BranchVars"     "regalloc_branch_vars"

echo ""
echo "Results: $PASS passed, $FAIL failed"

[ "$FAIL" -eq 0 ]
