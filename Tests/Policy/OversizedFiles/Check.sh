#!/usr/bin/env sh
# Oversized-file architecture guard.
#
# Hand-written compiler and unit-test implementation files should stay at or
# below 1,200 physical lines. A large file is not automatically bad, but growth
# beyond that point needs an explicit architecture review: splitting along a
# real ownership boundary is useful, while scattering a cohesive table or
# vendored source is not.
#
# Reviewed exceptions below are path-specific and capped at their reviewed line
# count. Raising a cap, adding a path, or changing its reason is therefore a
# visible policy change. An exception that reaches the ordinary limit must be
# removed so it cannot silently grow back.
#
# Run from anywhere: paths are resolved relative to the repository root.

set -eu

cd "$(dirname "$0")/../../.."

line_limit=1200

# Set reviewed_limit and reviewed_reason for a path whose current structure has
# been reviewed. These are deliberately individual files rather than directory
# patterns: new generated, vector, test, and third-party files receive the same
# review as any other implementation file.
review_exception() {
    reviewed_limit=
    reviewed_reason=

    case "$1" in
    Compiler/CodeGen/AArch64/RcuEmitter.cpp)
        reviewed_limit=1423
        reviewed_reason='cohesive module-level RCU sections, symbols, retry orchestration, fixups, and shared emitter hooks'
        ;;
    Compiler/Semantic/SemanticAnalyzer.cpp)
        reviewed_limit=3452
        reviewed_reason='central semantic phase orchestration, type resolution, declaration checking, and shared-context policy'
        ;;
    Tests/Unit/AArch64EncoderVectors.inc)
        reviewed_limit=1605
        reviewed_reason='dense one-line instruction reference vectors whose locality makes coverage auditable'
        ;;
    Tests/Unit/OptimizerTests.cpp)
        reviewed_limit=1284
        reviewed_reason='shared hand-built HIR/LIR pass fixtures and pipeline contract tests'
        ;;
    Tests/Unit/SemanticTests.cpp)
        reviewed_limit=1473
        reviewed_reason='end-to-end semantic contracts sharing source analysis and diagnostic-order assertions'
        ;;
    Tests/Unit/ThirdParty/doctest.h)
        reviewed_limit=7134
        reviewed_reason='vendored single-header doctest distribution; local splitting would fork the dependency'
        ;;
    esac
}

check_files() {
    checked=0
    reviewed=0
    violations=0

    while IFS= read -r file; do
        [ -n "$file" ] || continue
        checked=$((checked + 1))
        line_count=$(awk 'END { print NR + 0 }' "$file")
        review_exception "$file"

        if [ -n "$reviewed_limit" ]; then
            if [ "$line_count" -le "$line_limit" ]; then
                printf '%s: %s lines: error: reviewed oversized-file exception is stale; remove it from %s\n' \
                    "$file" "$line_count" "Tests/Policy/OversizedFiles/Check.sh" >&2
                violations=$((violations + 1))
            elif [ "$line_count" -gt "$reviewed_limit" ]; then
                printf '%s: %s lines: error: exceeds its reviewed ceiling of %s lines\n' \
                    "$file" "$line_count" "$reviewed_limit" >&2
                printf '  reviewed reason: %s\n' "$reviewed_reason" >&2
                violations=$((violations + 1))
            else
                printf 'reviewed oversized file: %s: %s lines (ceiling %s)\n' \
                    "$file" "$line_count" "$reviewed_limit"
                reviewed=$((reviewed + 1))
            fi
        elif [ "$line_count" -gt "$line_limit" ]; then
            printf '%s: %s lines: error: exceeds the architecture guideline of %s lines without a reviewed exception\n' \
                "$file" "$line_count" "$line_limit" >&2
            violations=$((violations + 1))
        fi
    done

    if [ "$violations" -ne 0 ]; then
        printf 'error: oversized-file architecture check found %s violation(s).\n' "$violations" >&2
        printf 'Split along a named ownership boundary, or add a path-specific reviewed ceiling and reason.\n' >&2
        return 1
    fi

    printf 'Oversized-file architecture check passed (%s files checked, %s reviewed exceptions).\n' \
        "$checked" "$reviewed"
}

# Headers and include fragments can contain implementation or dense tables, so
# they are checked with translation units. Sorting under the C locale makes both
# diagnostics and the success report deterministic on every host.
if ! find Compiler Tests/Unit -type f \( \
    -name '*.c' -o -name '*.cc' -o -name '*.cpp' -o -name '*.cxx' -o \
    -name '*.h' -o -name '*.hh' -o -name '*.hpp' -o -name '*.hxx' -o -name '*.inc' \
    \) -print | LC_ALL=C sort | check_files; then
    exit 1
fi
