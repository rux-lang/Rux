#!/usr/bin/env sh
# Reusable compiler components return data; process output belongs to CLI.
set -eu

script_directory=$(CDPATH= cd -P "$(dirname "$0")" && pwd)
repository_root=${1:-$(CDPATH= cd -P "$script_directory/../../.." && pwd)}
exceptions=$repository_root/Tests/Policy/OutputOwnership/Exceptions.txt

is_inspection_output() {
    while IFS='|' read -r pattern reason; do
        case "$pattern" in ''|'#'*) continue ;; esac
        [ -n "$reason" ] || continue
        case "$1" in $pattern) return 0 ;; esac
    done <"$exceptions"
    return 1
}

[ -f "$exceptions" ] || { printf 'error: missing inspection-output exceptions\n' >&2; exit 1; }
violations=$(
    grep -rnE 'std::(print|println|cout|cerr|clog)|(^|[^[:alnum:]_])(printf|fprintf|puts|fputs)[[:space:]]*\(' \
        "$repository_root/Compiler" --include='*.cpp' --include='*.h' 2>/dev/null |
        while IFS=: read -r file line text; do
            path=${file#"$repository_root"/}
            case "$path" in Compiler/Cli/*) continue ;; esac
            is_inspection_output "$path" && continue
            printf '%s:%s: error: return diagnostics or data to Compiler/Cli instead of writing process output\n' "$path" "$line"
        done
)
if [ -n "$violations" ]; then
    printf '%s\n' "$violations" >&2
    exit 1
fi
printf 'Output ownership check passed.\n'
