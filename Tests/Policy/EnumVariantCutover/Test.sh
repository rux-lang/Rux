#!/usr/bin/env sh
# Seeded positive and negative contracts for Check.sh.

set -eu

script_directory=$(CDPATH= cd -P "$(dirname "$0")" && pwd)
fixture_root=$(mktemp -d "${TMPDIR:-/tmp}/rux-enum-variant-cutover-policy.XXXXXX")
trap 'rm -rf "$fixture_root"' EXIT HUP INT TERM
policy=$script_directory/Check.sh
output=$fixture_root/output.txt

fail() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

reset_fixture() {
    rm -rf "$fixture_root/Packages" "$fixture_root/Tests"
    mkdir -p "$fixture_root/Packages/Example/Src" "$fixture_root/Tests/Language/Example/Src" \
        "$fixture_root/Tests/Packages/Example/Case"
    printf '%s\n' \
        'enum Month: uint8 {' \
        '    January = 1,' \
        '    February' \
        '}' \
        'variant Option<T> {' \
        '    Some(T),' \
        '    None' \
        '}' \
        'variant Message {' \
        '    Empty,' \
        '    Record { code: int32; text: char8[]; }' \
        '}' \
        >"$fixture_root/Packages/Example/Src/Main.rux"
    printf '%s\n' 'enum State { Idle, Ready }' \
        >"$fixture_root/Tests/Language/Example/Src/Main.rux"
    printf '%s\n' 'variant Pair { Values(int32, int32) }' \
        >"$fixture_root/Tests/Packages/Example/Case/Main.rux"
}

expect_pass() {
    sh "$policy" "$fixture_root" >"$output" 2>&1 || {
        sed 's/^/  /' "$output" >&2
        fail 'positive enum/variant-cutover fixture failed'
    }
    grep -F 'Enum and variant source cutover check passed.' "$output" >/dev/null || fail 'pass summary was missing'
}

expect_failure() {
    rule=$1
    if sh "$policy" "$fixture_root" >"$output" 2>&1; then
        fail "negative fixture for '$rule' passed"
    fi
    grep -F "policy rule $rule" "$output" >/dev/null || {
        sed 's/^/  /' "$output" >&2
        fail "negative fixture did not report rule '$rule'"
    }
    grep -F 'remediation:' "$output" >/dev/null || fail "'$rule' omitted remediation"
}

reset_fixture
expect_pass

reset_fixture
printf '%s\n' 'enum Generic<T> { Empty }' >>"$fixture_root/Packages/Example/Src/Main.rux"
expect_failure generic-enum

reset_fixture
printf '%s\n' 'enum Generic' '<T>' '{' '    Empty' '}' \
    >>"$fixture_root/Packages/Example/Src/Main.rux"
expect_failure generic-enum

reset_fixture
printf '%s\n' 'enum Result { Success(int32), Error(int32) }' \
    >>"$fixture_root/Packages/Example/Src/Main.rux"
expect_failure payload-enum

reset_fixture
printf '%s\n' 'enum Record {' '    Empty,' '    Value { item: int32; }' '}' \
    >>"$fixture_root/Packages/Example/Src/Main.rux"
expect_failure payload-enum

reset_fixture
printf '%s\n' 'enum Record { Empty, Value { item: int32; } }' \
    >>"$fixture_root/Packages/Example/Src/Main.rux"
expect_failure payload-enum

printf 'Enum and variant source cutover policy tests passed.\n'
