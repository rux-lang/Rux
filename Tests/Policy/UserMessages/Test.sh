#!/usr/bin/env sh
# Seeded positive and negative contracts for Check.sh.
set -eu

script_directory=$(CDPATH= cd -P "$(dirname "$0")" && pwd)
fixture_root=$(mktemp -d "${TMPDIR:-/tmp}/rux-user-message-policy.XXXXXX")
trap 'rm -rf "$fixture_root"' EXIT HUP INT TERM
policy=$script_directory/Check.sh
output=$fixture_root/output.txt

fail() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

reset_fixture() {
    rm -rf "$fixture_root/Compiler" "$fixture_root/Packages" "$fixture_root/Packaging" "$fixture_root/Scripts" \
        "$fixture_root/Tests"
    mkdir -p "$fixture_root/Compiler/Cli" "$fixture_root/Compiler/Reporting" \
        "$fixture_root/Compiler/Feature" "$fixture_root/Tests/Policy/UserMessages"
    printf '%s\n' 'Compiler/Feature/Inspection.cpp|inspection|Stable fixture payload.' \
        >"$fixture_root/Tests/Policy/UserMessages/Allowlist.txt"
    printf '%s\n' '#include <print>' 'void Report() { std::print(stderr, "error: failed\n"); }' \
        >"$fixture_root/Compiler/Cli/Reporter.cpp"
    printf '%s\n' '#include <string>' 'std::string Format() { return Reporting::FormatDuration({1}); }' \
        >"$fixture_root/Compiler/Feature/Message.cpp"
    printf '%s\n' '#include <print>' 'void Dump() { std::print("stable inspection"); }' \
        >"$fixture_root/Compiler/Feature/Inspection.cpp"
}

expect_pass() {
    sh "$policy" "$fixture_root" >"$output" 2>&1 || {
        sed 's/^/  /' "$output" >&2
        fail "positive user-message fixture failed"
    }
    grep -F 'User-message consistency check passed' "$output" >/dev/null || fail 'pass summary was missing'
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
    grep -F 'approved helper:' "$output" >/dev/null || fail "'$rule' omitted its approved helper"
    grep -F 'remediation:' "$output" >/dev/null || fail "'$rule' omitted remediation"
}

reset_fixture
expect_pass

reset_fixture
printf '%s\n' '#include <print>' 'void Leak() { std::print(stderr, "broken"); }' \
    >"$fixture_root/Compiler/Feature/Leak.cpp"
expect_failure direct-print

reset_fixture
printf '%s\n' 'const char *url = "https://rux-lang.dev/cli/build";' \
    >"$fixture_root/Compiler/Feature/Message.cpp"
expect_failure obsolete-cli-route

reset_fixture
printf '%s\n' 'const char *message = "Warning: broken";' >"$fixture_root/Compiler/Feature/Message.cpp"
expect_failure severity-prefix

reset_fixture
printf '%s\n' 'auto Text(auto elapsed) { return std::format("{} ms", elapsed.count()); }' \
    >"$fixture_root/Compiler/Feature/Message.cpp"
expect_failure hand-formatted-duration

reset_fixture
printf '%s\n' 'auto RenderJson() { return "\033[31m{}"; }' >"$fixture_root/Compiler/Feature/Json.cpp"
expect_failure ansi-in-json-owner

reset_fixture
printf '%s\n' 'const char *message = "help: install an external linker";' \
    >"$fixture_root/Compiler/Feature/Message.cpp"
expect_failure external-toolchain-suggestion

reset_fixture
printf '%s\n' 'Compiler/Feature/Inspection.cpp|inspection|' \
    >"$fixture_root/Tests/Policy/UserMessages/Allowlist.txt"
expect_failure unexplained-allowlist-entry

printf 'User-message policy fixture tests passed.\n'
