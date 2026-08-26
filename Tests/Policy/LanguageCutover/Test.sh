#!/usr/bin/env sh
# Seeded positive and negative contracts for Check.sh.
set -eu

script_directory=$(CDPATH= cd -P "$(dirname "$0")" && pwd)
fixture_root=$(mktemp -d "${TMPDIR:-/tmp}/rux-language-cutover-policy.XXXXXX")
trap 'rm -rf "$fixture_root"' EXIT HUP INT TERM
policy=$script_directory/Check.sh
output=$fixture_root/output.txt

fail() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

reset_fixture() {
    rm -rf "$fixture_root/Compiler" "$fixture_root/Packages" "$fixture_root/Tests"
    mkdir -p "$fixture_root/Compiler/Semantic" "$fixture_root/Compiler/Syntax/Parser" \
        "$fixture_root/Compiler/Lowering" "$fixture_root/Packages/Example/Src"
    printf '%s\n' "const char *message = \"requires an explicit '<-' in argument\";" \
        'if (RejectImplicitMove(expression, type, kind, location)) {}' \
        >"$fixture_root/Compiler/Semantic/SemanticMoveState.cpp"
    printf '%s\n' "const char *message = \"mutable parameter syntax 'var value: T' has been removed\";" \
        >"$fixture_root/Compiler/Syntax/Parser/ParserDeclDispatch.cpp"
    printf '%s\n' \
        'struct Value { field: int32; }' \
        'extend Value {' \
        '    func Value(field: int32) -> Value { return Value { field: field }; }' \
        '    func NewChecked(field: int32) -> Option<Value> { return Option::None<Value>(); }' \
        '    func ~Value(self: &var Value) {}' \
        '}' \
        >"$fixture_root/Packages/Example/Src/Main.rux"
}

expect_pass() {
    sh "$policy" "$fixture_root" >"$output" 2>&1 || {
        sed 's/^/  /' "$output" >&2
        fail 'positive language-cutover fixture failed'
    }
    grep -F 'Language ownership cutover check passed.' "$output" >/dev/null || fail 'pass summary was missing'
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
printf '%s\n' 'func Consume(var value: Value) {}' >>"$fixture_root/Packages/Example/Src/Main.rux"
expect_failure mutable-parameter

reset_fixture
printf '%s\n' 'extend Value : Drop { func Drop(self: &var Value) {} }' \
    >>"$fixture_root/Packages/Example/Src/Main.rux"
expect_failure legacy-lifecycle

reset_fixture
printf '%s\n' 'extend Value {' '    func New(field: int32) -> Value { return Value(field); }' '}' \
    >>"$fixture_root/Packages/Example/Src/Main.rux"
expect_failure exact-new-wrapper

reset_fixture
printf '%s\n' 'void ConsumeNamedValueWithoutCheckingCopyability();' \
    >"$fixture_root/Compiler/Semantic/SemanticMoveState.cpp"
expect_failure implicit-named-move

reset_fixture
printf '%s\n' 'void Restore(auto &parameter) { parameter.isMut = true; }' \
    >"$fixture_root/Compiler/Syntax/Ast.h"
expect_failure mutable-parameter-state

printf 'Language ownership cutover policy tests passed.\n'
