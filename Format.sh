#!/bin/sh

# Format all maintained C++ and Rux source code in the repository.
# Portable across Linux, macOS, and FreeBSD.

set -eu

check=false
rux_executable=

usage() {
    printf '%s\n' \
        'Usage: sh Format.sh [options]' \
        '' \
        'Options:' \
        '  --check                Check formatting without modifying files' \
        '  --rux-executable PATH  Existing rux executable (default: Bin/rux)' \
        '  -h, --help             Show this help'
}

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

require_value() {
    [ "$#" -ge 2 ] || die "option '$1' requires a value"
}

step() {
    printf '\n==> %s\n' "$1"
}

passed() {
    if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
        printf '\033[32m[PASSED]\033[0m %s\n' "$1"
    else
        printf '[PASSED] %s\n' "$1"
    fi
}

while [ "$#" -gt 0 ]; do
    case "$1" in
    --check)
        check=true
        shift
        ;;
    --rux-executable)
        require_value "$@"
        rux_executable=$2
        shift 2
        ;;
    -h | --help)
        usage
        exit 0
        ;;
    *)
        die "unknown option '$1'"
        ;;
    esac
done

script_directory=$(CDPATH= cd -P "$(dirname "$0")" && pwd)
repository_root=$script_directory

if [ -z "$rux_executable" ]; then
    rux_path=$repository_root/Bin/rux
else
    case "$rux_executable" in
    /*) rux_path=$rux_executable ;;
    *) rux_path=$repository_root/$rux_executable ;;
    esac
fi

[ -f "$rux_path" ] || die "Rux executable not found at '$rux_path'"

clang_format=
for candidate in clang-format-22 clang-format22 \
    /opt/homebrew/opt/llvm@22/bin/clang-format /usr/local/opt/llvm@22/bin/clang-format \
    /opt/homebrew/opt/llvm/bin/clang-format /usr/local/opt/llvm/bin/clang-format clang-format; do
    if clang_format=$(command -v "$candidate" 2>/dev/null); then
        break
    fi
    clang_format=
done
[ -n "$clang_format" ] || die "required tool not found: clang-format 22"

started_at=$(date +%s)
cd "$repository_root"

cpp_file_count=$(find Compiler Tests/Unit -type f \( -name '*.cpp' -o -name '*.h' \) \
    ! -path '*/ThirdParty/*' -print | wc -l | tr -d '[:space:]')
manifest_count=$(find Packages Tests -type f -name Rux.toml -print | wc -l | tr -d '[:space:]')

if [ "$check" = true ]; then
    step "Checking C++ formatting ($cpp_file_count files)"
    find Compiler Tests/Unit -type f \( -name '*.cpp' -o -name '*.h' \) \
        ! -path '*/ThirdParty/*' -exec "$clang_format" --dry-run -Werror {} +
    passed "C++ formatting ($cpp_file_count files)"

    step "Checking Rux formatting ($manifest_count packages)"
    find Packages Tests -type f -name Rux.toml -print | while IFS= read -r manifest; do
        "$rux_path" --manifest "$manifest" fmt --check
    done
    passed "Rux formatting ($manifest_count packages)"
else
    step "Formatting C++ sources ($cpp_file_count files)"
    find Compiler Tests/Unit -type f \( -name '*.cpp' -o -name '*.h' \) \
        ! -path '*/ThirdParty/*' -exec "$clang_format" -i {} +
    passed "C++ formatting ($cpp_file_count files)"

    step "Formatting Rux sources ($manifest_count packages)"
    find Packages Tests -type f -name Rux.toml -print | while IFS= read -r manifest; do
        "$rux_path" --manifest "$manifest" fmt
    done
    passed "Rux formatting ($manifest_count packages)"
fi

elapsed=$(( $(date +%s) - started_at ))
if [ "$check" = true ]; then
    action=check
else
    action=formatting
fi
printf '\nSource %s passed in %02d:%02d.\n' "$action" "$((elapsed / 60))" "$((elapsed % 60))"
