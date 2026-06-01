#!/usr/bin/env bash
# Build Tests/Dll and verify a .dll artifact exists (Windows hosts only).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUX="${RUX:-${1:-}}"
if [ -z "$RUX" ]; then
    for candidate in \
        "$SCRIPT_DIR/../build/rux" \
        "$SCRIPT_DIR/../build/rux.exe" \
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

TEST_DIR="${SCRIPT_DIR}/Dll"

cd "${TEST_DIR}"
"${RUX}" build
PROFILE="${RUX_PROFILE:-Debug}"
DLL="${TEST_DIR}/Bin/${PROFILE}/dll_test.dll"
if [[ ! -f "${DLL}" ]]; then
    echo "error: expected DLL at ${DLL}" >&2
    exit 1
fi
echo "ok: ${DLL}"
