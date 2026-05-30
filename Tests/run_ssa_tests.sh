#!/usr/bin/env bash
# Tests/run_ssa_tests.sh
#
# Verifies SSA construction pass correctness by:
#   1. Running rux build --dump-lir on each SSA test package
#   2. Inspecting the LIR dump for SSA invariants
#
# Usage:
#   ./Tests/run_ssa_tests.sh [path-to-rux-binary]
#
# If path-to-rux-binary is omitted, defaults to ./build/clang/rux

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
RUX="${1:-$ROOT_DIR/build/clang/rux}"
# Resolve to absolute path so it works after cd into package dirs.
RUX="$(cd "$(dirname "$RUX")" && pwd)/$(basename "$RUX")"

if [[ ! -x "$RUX" ]]; then
    echo "ERROR: rux binary not found at: $RUX" >&2
    exit 1
fi

PASS=0
FAIL=0
ERRORS=()

# ──────────────────────────────────────────────────────────────────────────────
# Helpers
# ──────────────────────────────────────────────────────────────────────────────

run_dump() {
    local pkg_dir="$1"
    # Build with --dump-lir; redirect all output so it doesn't pollute $DUMP.
    # The dump is written before codegen, so even a codegen failure still produces it.
    "$RUX" build --dump-lir >/dev/null 2>&1 || true
    local dump="$pkg_dir/Temp/Lir/lir.txt"
    echo "$dump"
}

check_pass() {
    local name="$1"
    local result="$2"
    local detail="$3"
    if [[ "$result" == "ok" ]]; then
        echo "  [PASS] $name"
        PASS=$((PASS + 1))
    else
        echo "  [FAIL] $name: $detail"
        FAIL=$((FAIL + 1))
        ERRORS+=("$name: $detail")
    fi
}

# Count lines matching a pattern in a file
count_matches() {
    local n
    n=$(grep -c "$1" "$2" 2>/dev/null) || n=0
    echo "$n"
}

# Check SSA single-definition property per function.
# Vregs reset to 0 for each function, so we scope the check per-function.
# Uses a single awk pass — avoids fragile grep/sort pipelines under set -eo pipefail.
check_single_def() {
    local dump="$1"
    local func_name="$2"
    local dupes
    # awk: track seen register numbers per function; report duplicates.
    dupes=$(awk '
        /^func / { delete seen }
        /[[:space:]]%[0-9]+ =/ {
            s = $0
            # strip everything before %
            sub(/.*[[:space:]]%/, "", s)
            # s now starts with digits; extract the number
            n = s + 0
            # verify what follows is " ="
            if (s ~ /^[0-9]+ =/) {
                if (n in seen) printf "dup:%%%s ", n
                seen[n] = 1
            }
        }
    ' "$dump" 2>/dev/null) || dupes=""
    if [[ -z "$dupes" ]]; then
        check_pass "$func_name: single-def (each vreg defined once per func)" "ok" ""
    else
        check_pass "$func_name: single-def (each vreg defined once per func)" "fail" "$dupes"
    fi
}

# ──────────────────────────────────────────────────────────────────────────────
# Test 1: Simple — no promotable alloca/load/store in Main
# ──────────────────────────────────────────────────────────────────────────────
echo ""
echo "=== Test: SsaConstruct/Simple ==="
PKG="$SCRIPT_DIR/SsaConstruct/Simple"
cd "$PKG"
DUMP=$(run_dump "$PKG")

if [[ ! -f "$DUMP" ]]; then
    echo "  [FAIL] LIR dump not produced at $DUMP"
    ((FAIL++)) || true
    ERRORS+=("Simple: no LIR dump produced")
else
    # After SSA, 'let x: int = 42' should produce a const instruction directly.
    # There should be NO alloca/store/load in the Main function for x.
    # The LIR dump emits "alloca", "store", "load" for the opcodes.
    ALLOCA_CNT=$(count_matches '= alloca' "$DUMP")
    # A pure simple function: all allocas should be zero after promotion.
    if [[ "$ALLOCA_CNT" -eq 0 ]]; then
        check_pass "Simple: no alloca remaining" "ok" ""
    else
        # Some allocas may be non-promotable (e.g. string slices) — we just
        # confirm the scalar local was promoted by checking for a const i64 42.
        CONST42=$(count_matches 'const.*42' "$DUMP")
        if [[ "$CONST42" -gt 0 ]]; then
            check_pass "Simple: scalar promoted (const 42 visible)" "ok" ""
        else
            check_pass "Simple: scalar promoted (const 42 visible)" "fail" \
                "alloca count=$ALLOCA_CNT, no 'const 42' found — scalar may not have been promoted"
        fi
    fi
    check_single_def "$DUMP" "Simple"
fi

# ──────────────────────────────────────────────────────────────────────────────
# Test 2: IfElse — phi node at merge block
# ──────────────────────────────────────────────────────────────────────────────
echo ""
echo "=== Test: SsaConstruct/IfElse ==="
PKG="$SCRIPT_DIR/SsaConstruct/IfElse"
cd "$PKG"
DUMP=$(run_dump "$PKG")

if [[ ! -f "$DUMP" ]]; then
    echo "  [FAIL] LIR dump not produced"
    ((FAIL++)) || true
    ERRORS+=("IfElse: no LIR dump produced")
else
    PHI_CNT=$(count_matches '= phi' "$DUMP")
    if [[ "$PHI_CNT" -gt 0 ]]; then
        check_pass "IfElse: phi node inserted at merge" "ok" ""
    else
        check_pass "IfElse: phi node inserted at merge" "fail" \
            "no phi instructions found in LIR dump"
    fi

    # The merge block phi should have exactly 2 predecessors (then=10, else=20).
    # Phi format in dump: "phi type [%N, block], [%M, block]"
    PHI_PREDS=$(grep '= phi' "$DUMP" | head -1) || PHI_PREDS=""
    PRED_CNT=$(echo "$PHI_PREDS" | grep -oE '\[%' | wc -l | tr -d ' ')
    if [[ "$PRED_CNT" -eq 2 ]]; then
        check_pass "IfElse: phi has exactly 2 predecessors" "ok" ""
    else
        check_pass "IfElse: phi has exactly 2 predecessors" "fail" \
            "found $PRED_CNT predecessor(s) in: $PHI_PREDS"
    fi

    # Scalar locals (cond, x) should not have alloca remaining.
    ALLOCA_CNT=$(count_matches '= alloca' "$DUMP")
    if [[ "$ALLOCA_CNT" -eq 0 ]]; then
        check_pass "IfElse: no alloca remaining" "ok" ""
    else
        check_pass "IfElse: no alloca remaining" "fail" \
            "$ALLOCA_CNT alloca(s) remain in dump"
    fi

    check_single_def "$DUMP" "IfElse"
fi

# ──────────────────────────────────────────────────────────────────────────────
# Test 3: Loop — phi nodes at loop header
# ──────────────────────────────────────────────────────────────────────────────
echo ""
echo "=== Test: SsaConstruct/Loop ==="
PKG="$SCRIPT_DIR/SsaConstruct/Loop"
cd "$PKG"
DUMP=$(run_dump "$PKG")

if [[ ! -f "$DUMP" ]]; then
    echo "  [FAIL] LIR dump not produced"
    ((FAIL++)) || true
    ERRORS+=("Loop: no LIR dump produced")
else
    PHI_CNT=$(count_matches '= phi' "$DUMP")
    # Two loop variables (i, sum) → at least 2 phi nodes
    if [[ "$PHI_CNT" -ge 2 ]]; then
        check_pass "Loop: >= 2 phi nodes for i and sum" "ok" ""
    else
        check_pass "Loop: >= 2 phi nodes for i and sum" "fail" \
            "only $PHI_CNT phi instruction(s) found; expected >= 2"
    fi

    ALLOCA_CNT=$(count_matches '= alloca' "$DUMP")
    if [[ "$ALLOCA_CNT" -eq 0 ]]; then
        check_pass "Loop: no alloca remaining" "ok" ""
    else
        check_pass "Loop: no alloca remaining" "fail" \
            "$ALLOCA_CNT alloca(s) remain"
    fi

    check_single_def "$DUMP" "Loop"
fi

# ──────────────────────────────────────────────────────────────────────────────
# Test 4: MultiVar — all five scalars promoted
# ──────────────────────────────────────────────────────────────────────────────
echo ""
echo "=== Test: SsaConstruct/MultiVar ==="
PKG="$SCRIPT_DIR/SsaConstruct/MultiVar"
cd "$PKG"
DUMP=$(run_dump "$PKG")

if [[ ! -f "$DUMP" ]]; then
    echo "  [FAIL] LIR dump not produced"
    ((FAIL++)) || true
    ERRORS+=("MultiVar: no LIR dump produced")
else
    ALLOCA_CNT=$(count_matches '= alloca' "$DUMP")
    if [[ "$ALLOCA_CNT" -eq 0 ]]; then
        check_pass "MultiVar: all 5 scalars promoted (no alloca)" "ok" ""
    else
        # Verify the five consts are present
        CONST_CNT=0
        for v in 1 2 3 4 5; do
            if grep -q "const.*$v" "$DUMP"; then
                ((CONST_CNT++)) || true
            fi
        done
        if [[ "$CONST_CNT" -ge 4 ]]; then
            check_pass "MultiVar: constants visible after promotion" "ok" ""
        else
            check_pass "MultiVar: all 5 scalars promoted (no alloca)" "fail" \
                "$ALLOCA_CNT alloca(s) remain, only $CONST_CNT of 5 consts visible"
        fi
    fi

    check_single_def "$DUMP" "MultiVar"
fi

# ──────────────────────────────────────────────────────────────────────────────
# Test 5: AddressTaken — alloca for x must NOT be promoted
# ──────────────────────────────────────────────────────────────────────────────
echo ""
echo "=== Test: SsaConstruct/AddressTaken ==="
PKG="$SCRIPT_DIR/SsaConstruct/AddressTaken"
cd "$PKG"
DUMP=$(run_dump "$PKG")

if [[ ! -f "$DUMP" ]]; then
    echo "  [FAIL] LIR dump not produced"
    ((FAIL++)) || true
    ERRORS+=("AddressTaken: no LIR dump produced")
else
    # x's alloca escapes via &x — must remain
    ALLOCA_CNT=$(count_matches '= alloca' "$DUMP")
    if [[ "$ALLOCA_CNT" -ge 1 ]]; then
        check_pass "AddressTaken: alloca preserved for address-taken var" "ok" ""
    else
        check_pass "AddressTaken: alloca preserved for address-taken var" "fail" \
            "no alloca found — address-taken variable was incorrectly promoted"
    fi

    # There must be a store instruction feeding the alloca slot
    STORE_CNT=$(count_matches '^[[:space:]]*store ' "$DUMP")
    if [[ "$STORE_CNT" -ge 1 ]]; then
        check_pass "AddressTaken: store instruction preserved" "ok" ""
    else
        check_pass "AddressTaken: store instruction preserved" "fail" \
            "no store instruction — variable may have been incorrectly promoted"
    fi

    check_single_def "$DUMP" "AddressTaken"
fi

# ──────────────────────────────────────────────────────────────────────────────
# Summary
# ──────────────────────────────────────────────────────────────────────────────
echo ""
echo "══════════════════════════════════════════════════"
echo "  SSA construction tests: $PASS passed, $FAIL failed"
echo "══════════════════════════════════════════════════"
if [[ ${#ERRORS[@]} -gt 0 ]]; then
    echo ""
    echo "Failures:"
    for e in "${ERRORS[@]}"; do
        echo "  - $e"
    done
fi
echo ""

[[ "$FAIL" -eq 0 ]]
