#!/usr/bin/env sh
# Contract tests for the oversized-file architecture guard.

set -eu

script_directory=$(CDPATH= cd -P "$(dirname "$0")" && pwd)
fixture_root=$(mktemp -d "${TMPDIR:-/tmp}/rux-oversized-files.XXXXXX")
trap 'rm -rf "$fixture_root"' EXIT HUP INT TERM

mkdir -p "$fixture_root/Compiler" "$fixture_root/Tests/Policy/OversizedFiles" "$fixture_root/Tests/Unit"
cp "$script_directory/Check.sh" "$fixture_root/Tests/Policy/OversizedFiles/Check.sh"

output=$fixture_root/output.txt

write_lines() {
    destination=$1
    count=$2
    mkdir -p "$(dirname "$destination")"
    awk -v count="$count" 'BEGIN { for (line = 1; line <= count; ++line) print "// fixture line " line }' \
        >"$destination"
}

run_check() {
    sh "$fixture_root/Tests/Policy/OversizedFiles/Check.sh" >"$output" 2>&1
}

require_output() {
    if ! grep -F "$1" "$output" >/dev/null; then
        printf 'error: expected policy output containing: %s\n' "$1" >&2
        printf '%s\n' '--- policy output ---' >&2
        sed 's/^/  /' "$output" >&2
        exit 1
    fi
}

write_lines "$fixture_root/Compiler/Small.cpp" 1200
if ! run_check; then
    printf 'error: a file at the ordinary limit should pass\n' >&2
    exit 1
fi
require_output 'Oversized-file architecture check passed'

write_lines "$fixture_root/Compiler/Oversized.cpp" 1201
if run_check; then
    printf 'error: an unreviewed oversized file should fail\n' >&2
    exit 1
fi
require_output 'Compiler/Oversized.cpp: 1201 lines: error:'
require_output 'without a reviewed exception'
rm "$fixture_root/Compiler/Oversized.cpp"

write_lines "$fixture_root/Compiler/CodeGen/AArch64/RcuEmitter.cpp" 1500
if ! run_check; then
    printf 'error: a reviewed file at its ceiling should pass\n' >&2
    exit 1
fi
require_output 'reviewed oversized file: Compiler/CodeGen/AArch64/RcuEmitter.cpp: 1500 lines (ceiling 1500)'

write_lines "$fixture_root/Compiler/CodeGen/AArch64/RcuEmitter.cpp" 1501
if run_check; then
    printf 'error: growth beyond a reviewed ceiling should fail\n' >&2
    exit 1
fi
require_output 'Compiler/CodeGen/AArch64/RcuEmitter.cpp: 1501 lines: error: exceeds its reviewed ceiling of 1500 lines'

write_lines "$fixture_root/Compiler/CodeGen/AArch64/RcuEmitter.cpp" 1200
if run_check; then
    printf 'error: an exception should be removed after its file reaches the ordinary limit\n' >&2
    exit 1
fi
require_output 'reviewed oversized-file exception is stale'
rm "$fixture_root/Compiler/CodeGen/AArch64/RcuEmitter.cpp"

write_lines "$fixture_root/Tests/Unit/ThirdParty/Other.h" 1201
if run_check; then
    printf 'error: third-party directories must not receive blanket exceptions\n' >&2
    exit 1
fi
require_output 'Tests/Unit/ThirdParty/Other.h: 1201 lines: error:'

printf 'Oversized-file architecture policy tests passed.\n'
