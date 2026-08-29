#!/usr/bin/env sh
# Final enum/variant source guard.
#
# Positive first-party source must use `enum` only for scalar constants and `variant` for closed case types. The
# compiler owns the full grammar and diagnostics; this guard prevents a future compatibility relaxation or copied
# example from quietly restoring generic or payload-bearing enums in the maintained source roots.

set -eu

script_directory=$(CDPATH= cd -P "$(dirname "$0")" && pwd)
repository_root=${1:-$(CDPATH= cd -P "$script_directory/../../.." && pwd)}
audit=$(mktemp "${TMPDIR:-/tmp}/rux-enum-variant-cutover.XXXXXX")
trap 'rm -f "$audit"' EXIT HUP INT TERM

cd "$repository_root"

report() {
    file=$1
    line=$2
    rule=$3
    remediation=$4
    printf '%s:%s: error: enum-variant-cutover policy rule %s\n' "$file" "$line" "$rule" >>"$audit"
    printf '  remediation: %s\n' "$remediation" >>"$audit"
}

source_roots='Packages Tests/Language Tests/Packages'
for root in $source_roots; do
    [ -d "$root" ] || continue
    find "$root" -type f -name '*.rux' -exec awk '
        function braceDelta(text, opened, closed) {
            opened = gsub(/\{/, "{", text)
            closed = gsub(/\}/, "}", text)
            return opened - closed
        }
        function inspectDeclaration(compact, body) {
            if (header ~ /enum[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[[:space:]]*</) {
                print FILENAME ":" start ":generic-enum"
            }
            compact = declaration
            gsub(/[[:space:]]/, "", compact)
            sub(/^.*enum[A-Za-z_][A-Za-z0-9_]*(<[^>]*>)?(:[^{]+)?\{/, "", compact)
            sub(/\}[^}]*$/, "", compact)
            body = compact
            if (body ~ /(^|,)[A-Za-z_][A-Za-z0-9_]*\(/ ||
                body ~ /(^|,)[A-Za-z_][A-Za-z0-9_]*\{/) {
                print FILENAME ":" start ":payload-enum"
            }
        }
        {
            line = $0
            sub(/\/\/.*/, "", line)
        }
        !inside && line ~ /^[[:space:]]*(pub[[:space:]]+)?enum[[:space:]]+[A-Za-z_][A-Za-z0-9_]*/ {
            inside = 1
            start = FNR
            header = line
            declaration = line
            depth = braceDelta(line)
            if (depth <= 0 && line ~ /\}/) {
                inspectDeclaration()
                inside = 0
            }
            next
        }
        inside {
            declaration = declaration " " line
            if (header !~ /\{/) {
                header = header " " line
            }
            depth += braceDelta(line)
            if (depth <= 0 && declaration ~ /\}/) {
                inspectDeclaration()
                inside = 0
            }
        }
    ' {} +
done | LC_ALL=C sort | while IFS=: read -r file line rule; do
    case "$rule" in
    generic-enum)
        report "$file" "$line" "$rule" \
            "replace the declaration with 'variant' when its cases depend on type parameters"
        ;;
    payload-enum)
        report "$file" "$line" "$rule" \
            "replace the declaration with 'variant'; enum members cannot carry tuple or named payloads"
        ;;
    esac
done

if [ -s "$audit" ]; then
    cat "$audit" >&2
    exit 1
fi

printf 'Enum and variant source cutover check passed.\n'
