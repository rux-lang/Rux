#!/usr/bin/env sh
# Final ownership/lifecycle source guard.
#
# Positive first-party Rux sources must use the final parameter, constructor, move, and destructor model. The
# semantic compiler remains the authority for whether a value is copyable; this guard also pins the parser diagnostic
# and semantic rejection path that make removed mutable parameters and implicit named moves errors, so deleting either
# cutover cannot make old source silently valid again.

set -eu

script_directory=$(CDPATH= cd -P "$(dirname "$0")" && pwd)
repository_root=${1:-$(CDPATH= cd -P "$script_directory/../../.." && pwd)}
audit=$(mktemp "${TMPDIR:-/tmp}/rux-language-cutover.XXXXXX")
trap 'rm -f "$audit"' EXIT HUP INT TERM

cd "$repository_root"

report() {
    file=$1
    line=$2
    rule=$3
    remediation=$4
    printf '%s:%s: error: language-cutover policy rule %s\n' "$file" "$line" "$rule" >>"$audit"
    printf '  remediation: %s\n' "$remediation" >>"$audit"
}

source_roots='Packages Tests/Language Tests/Packages'
for root in $source_roots; do
    [ -d "$root" ] || continue
    find "$root" -type f -name '*.rux' -exec awk '
        FNR == 1 {
            parameterSignature = ""
            parameterStart = 0
            newSignature = ""
            newStart = 0
            extended = ""
        }
        /^[[:space:]]*extend[[:space:]]+/ {
            extended = $2
            sub(/[{:].*$/, "", extended)
        }
        /^[[:space:]]*(pub[[:space:]]+)?(extern[[:space:]]+|asm[[:space:]]+|intrinsic[[:space:]]+)*func[[:space:]]/ {
            parameterSignature = $0
            parameterStart = FNR
        }
        parameterSignature != "" && FNR != parameterStart {
            parameterSignature = parameterSignature " " $0
        }
        parameterSignature != "" && (index($0, "{") || index($0, ";")) {
            if (parameterSignature ~ /(^|[,([:space:]])var[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[[:space:]]*:/) {
                print FILENAME ":" parameterStart ":mutable-parameter"
            }
            parameterSignature = ""
        }
        /^[[:space:]]*(pub[[:space:]]+)?func[[:space:]]+New[[:space:]]*\(/ {
            newSignature = $0
            newStart = FNR
        }
        newSignature != "" && FNR != newStart { newSignature = newSignature " " $0 }
        newSignature != "" && (index($0, "{") || index($0, ";")) {
            compact = newSignature
            gsub(/[[:space:]]/, "", compact)
            if (extended != "" && index(compact, "funcNew(") && index(compact, "->" extended) &&
                compact !~ /error:/ && compact !~ /->Option</) {
                print FILENAME ":" newStart ":exact-new-wrapper"
            }
            newSignature = ""
        }
        /Core::Drop|extend[[:space:]]+[^\{]+:[[:space:]]*Drop([[:space:]]|\{|$)|func[[:space:]]+Drop[[:space:]]*\(/ {
            print FILENAME ":" FNR ":legacy-lifecycle"
        }
    ' {} +
done | LC_ALL=C sort | while IFS=: read -r file line rule; do
    case "$rule" in
    mutable-parameter)
        report "$file" "$line" "$rule" \
            "write 'name: T', then move into a mutable local or accept 'name: &var T'"
        ;;
    legacy-lifecycle)
        report "$file" "$line" "$rule" \
            "replace lifecycle interfaces or Drop methods with 'func ~Type(self: &var Type)'"
        ;;
    exact-new-wrapper)
        report "$file" "$line" "$rule" \
            "rename an infallible exact-type New factory to the extended type and call it as 'Type(...)'"
        ;;
    esac
done

if ! grep -F "requires an explicit '<-' in" Compiler/Semantic/SemanticMoveState.cpp >/dev/null 2>&1; then
    report Compiler/Semantic/SemanticMoveState.cpp 1 implicit-named-move \
        "keep plain named by-value use as copy and diagnose move-only sources without '<-'"
fi

if ! grep -F 'if (RejectImplicitMove(expression, type, kind, location))' \
    Compiler/Semantic/SemanticMoveState.cpp >/dev/null 2>&1; then
    report Compiler/Semantic/SemanticMoveState.cpp 1 implicit-named-move \
        "keep the move-only consumption path connected to the implicit-move rejection"
fi

if ! grep -F "mutable parameter syntax '" Compiler/Syntax/Parser/ParserDeclDispatch.cpp >/dev/null 2>&1; then
    report Compiler/Syntax/Parser/ParserDeclDispatch.cpp 1 mutable-parameter-diagnostic \
        "keep the parser rejection and migration help for 'var name: T' parameters"
fi

mutable_state=$(grep -R -nE 'param(eter)?\.isMut' \
    Compiler/Syntax Compiler/Semantic Compiler/Lowering --include='*.cpp' --include='*.h' 2>/dev/null || true)
if [ -n "$mutable_state" ]; then
    printf '%s\n' "$mutable_state" >>"$audit"
    report Compiler/Syntax/Ast/Ast.h 1 mutable-parameter-state \
        "do not restore mutable-parameter state; parameter bindings are immutable"
fi

if [ -s "$audit" ]; then
    cat "$audit" >&2
    exit 1
fi

printf 'Language ownership cutover check passed.\n'
