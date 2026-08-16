#!/usr/bin/env sh
# Repository-wide user-message ownership and style guard.
set -eu

script_directory=$(CDPATH= cd -P "$(dirname "$0")" && pwd)
repository_root=${1:-$(CDPATH= cd -P "$script_directory/../../.." && pwd)}
allowlist=${RUX_USER_MESSAGE_ALLOWLIST:-$repository_root/Tests/Policy/UserMessages/Allowlist.txt}
violations=0

fail() {
    file=$1
    line=$2
    rule=$3
    helper=$4
    remediation=$5
    printf '%s:%s: error: user-message policy rule %s\n' "$file" "$line" "$rule" >&2
    printf '  approved helper: %s\n' "$helper" >&2
    printf '  remediation: %s\n' "$remediation" >&2
    violations=$((violations + 1))
}

relative_path() {
    case "$1" in
        "$repository_root"/*) printf '%s' "${1#"$repository_root"/}" ;;
        *) printf '%s' "$1" ;;
    esac
}

is_allowlisted() {
    candidate=$1
    [ -f "$allowlist" ] || return 1
    while IFS='|' read -r pattern kind reason; do
        case "$pattern" in ''|'#'*) continue ;; esac
        [ -n "$kind" ] && [ -n "$reason" ] || continue
        case "$candidate" in $pattern) return 0 ;; esac
    done <"$allowlist"
    return 1
}

validate_allowlist() {
    [ -f "$allowlist" ] || {
        fail "Tests/Policy/UserMessages/Allowlist.txt" 1 "missing-allowlist" \
            "a path-specific compatibility entry with a reason" \
            "restore the allowlist and document every stable output exception"
        return
    }
    line_number=0
    while IFS='|' read -r pattern kind reason; do
        line_number=$((line_number + 1))
        case "$pattern" in ''|'#'*) continue ;; esac
        if [ -z "$kind" ] || [ -z "$reason" ]; then
            fail "Tests/Policy/UserMessages/Allowlist.txt" "$line_number" "unexplained-allowlist-entry" \
                "path pattern|kind|reason" "add both a compatibility category and a concrete reason"
        fi
    done <"$allowlist"
}

scan_direct_printing() {
    [ -d "$repository_root/Compiler" ] || return
    grep -rnE 'std::(print|println|cout|cerr|clog)|(^|[^[:alnum:]_])(printf|fprintf|puts|fputs)[[:space:]]*\(' \
        "$repository_root/Compiler" --include='*.cpp' --include='*.h' 2>/dev/null |
        while IFS=: read -r file line rest; do
        path=$(relative_path "$file")
        case "$path" in Compiler/Cli/*) continue ;; esac
        is_allowlisted "$path" && continue
        fail "$path" "$line" "direct-print" "CliSupport::Reporter or a structured failure/Diagnostic" \
            "return data to Compiler/Cli; add only stable inspection payloads to the reasoned allowlist"
    done
}

scan_one_message_file() {
    file=$1
    path=$(relative_path "$file")
    grep -nF 'rux-lang.dev/cli' "$file" 2>/dev/null | while IFS=: read -r line rest; do
        fail "$path" "$line" "obsolete-cli-route" "CliContract::DocumentationUrl" \
            "use a verified 'https://rux-lang.dev/docs/cli' route"
    done
    grep -nE '"[^"]*(Error|Warning|Note|Help|Docs):[[:space:]]' "$file" 2>/dev/null |
        while IFS=: read -r line rest; do
            fail "$path" "$line" "severity-prefix" "Reporter Error/Warning/Note/Help/Link methods" \
                "spell canonical severity prefixes in lowercase"
        done
}

scan_bad_routes_and_severity() {
    for area in Compiler Packages Packaging Scripts; do
        [ -d "$repository_root/$area" ] || continue
        grep -rlE 'rux-lang\.dev/cli|"[^"]*(Error|Warning|Note|Help|Docs):[[:space:]]' "$repository_root/$area" \
            --include='*.cpp' --include='*.h' --include='*.rux' --include='*.ps1' --include='*.sh' 2>/dev/null |
            while IFS= read -r file; do scan_one_message_file "$file"; done
    done
    for script in Build.sh Format.sh Test.sh Build.ps1 Format.ps1 Test.ps1; do
        [ -f "$repository_root/$script" ] && scan_one_message_file "$repository_root/$script"
    done
}

scan_durations() {
    [ -d "$repository_root/Compiler" ] || return
    grep -rnE '(elapsed|duration)\.count[[:space:]]*\(|duration_cast<std::chrono::(seconds|milliseconds)>|\{[^}]*\}[[:space:]]*(ms|min|sec(ond)?s?)\b' \
        "$repository_root/Compiler" --include='*.cpp' --include='*.h' 2>/dev/null |
        while IFS=: read -r file line rest; do
        path=$(relative_path "$file")
        case "$path" in
            Compiler/Reporting/Reporting.cpp|Compiler/Driver/BuildReport.h) continue ;;
        esac
        fail "$path" "$line" "hand-formatted-duration" "Reporting::FormatDuration" \
            "pass elapsed milliseconds to the canonical formatter"
    done
}

scan_json_ansi() {
    [ -d "$repository_root/Compiler" ] || return
    grep -ril 'json' "$repository_root/Compiler" --include='*.cpp' --include='*.h' 2>/dev/null |
        while IFS= read -r file; do
        path=$(relative_path "$file")
        grep -nE '\\033|\\x1[bB]|Ansi::' "$file" 2>/dev/null | while IFS=: read -r line rest; do
            fail "$path" "$line" "ansi-in-json-owner" "plain JSON renderer plus resolved human-only Style" \
                "keep ANSI tokens out of every file that owns JSON serialization"
        done
    done
}

scan_toolchain_suggestions() {
    [ -d "$repository_root/Compiler" ] || return
    grep -rniE '"[^"\n]*(help:[[:space:]]|try |install |use ).*(assembler|compiler|toolchain|linker|clang|gcc|lld)' \
        "$repository_root/Compiler" --include='*.cpp' --include='*.h' 2>/dev/null |
        while IFS=: read -r file line rest; do
        path=$(relative_path "$file")
        fail "$path" "$line" "external-toolchain-suggestion" "an in-process Rux recovery action" \
            "describe the Rux input or target correction without recommending an external toolchain"
    done
}

# Capture the complete audit because POSIX pipeline bodies run in subshells;
# their diagnostics are the deterministic failure signal on every shell.
audit_output=$(mktemp "${TMPDIR:-/tmp}/rux-user-message-audit.XXXXXX")
trap 'rm -f "$audit_output"' EXIT HUP INT TERM
set +e
{
    validate_allowlist
    scan_direct_printing
    scan_bad_routes_and_severity
    scan_durations
    scan_json_ansi
    scan_toolchain_suggestions
} 2>"$audit_output"
set -e
if [ -s "$audit_output" ]; then
    cat "$audit_output" >&2
    count=$(grep -c 'error: user-message policy rule' "$audit_output" || true)
    printf 'error: user-message consistency check found %s violation(s).\n' "$count" >&2
    exit 1
fi

entries=$(grep -cE '^[^#][^|]*\|[^|]+\|.+' "$allowlist" || true)
printf 'User-message consistency check passed (%s reasoned compatibility entries).\n' "$entries"
