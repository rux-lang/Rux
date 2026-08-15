#!/bin/sh

# Format all maintained C++ and Rux source code in the repository.
# Portable across Linux, macOS, and FreeBSD.

set -eu

script_directory=$(CDPATH= cd -P "$(dirname "$0")" && pwd)
. "$script_directory/Scripts/RepositoryMessages.sh"

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

require_value() {
    [ "$#" -ge 2 ] || die "option '$1' requires a value"
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

repository_root=$script_directory

if [ -z "$rux_executable" ]; then
    rux_path=$repository_root/Bin/rux
else
    case "$rux_executable" in
    /*) rux_path=$rux_executable ;;
    *) rux_path=$repository_root/$rux_executable ;;
    esac
fi

[ -f "$rux_path" ] || die "rux executable '$rux_path' was not found; build it first or pass --rux-executable"

clang_format=
for candidate in clang-format-22 clang-format22 \
    /opt/homebrew/opt/llvm@22/bin/clang-format /usr/local/opt/llvm@22/bin/clang-format \
    /opt/homebrew/opt/llvm/bin/clang-format /usr/local/opt/llvm/bin/clang-format clang-format; do
    if clang_format=$(command -v "$candidate" 2>/dev/null); then
        break
    fi
    clang_format=
done
[ -n "$clang_format" ] || die "required tool 'clang-format 22' was not found; install it and ensure it is available on PATH"

started_at=$(date +%s)
cd "$repository_root"

cpp_file_count=$(find Compiler Tests/Unit -type f \( -name '*.cpp' -o -name '*.h' \) \
    ! -path '*/ThirdParty/*' -print | wc -l | tr -d '[:space:]')
manifest_count=$(find Packages Tests -type f -name Rux.toml -print | wc -l | tr -d '[:space:]')

[ "$cpp_file_count" -gt 0 ] || die "no maintained C++ files were found"
[ "$manifest_count" -gt 0 ] || die "no Rux package or test manifests were found"

if [ "$check" = true ]; then
    step "Checking C++ formatting ($cpp_file_count files)"
    if ! find Compiler Tests/Unit -type f \( -name '*.cpp' -o -name '*.h' \) ! -path '*/ThirdParty/*' -print0 |
        xargs -0 "$clang_format" --dry-run -Werror; then
        die "command 'clang-format' failed"
    fi
    passed "C++ formatting ($cpp_file_count files)"

    step "Checking Rux formatting ($manifest_count packages)"
    find Packages Tests -type f -name Rux.toml -print | while IFS= read -r manifest; do
        run_checked rux "$rux_path" --manifest "$manifest" fmt --check
    done
    passed "Rux formatting ($manifest_count packages)"
else
    step "Formatting C++ sources ($cpp_file_count files)"
    if ! find Compiler Tests/Unit -type f \( -name '*.cpp' -o -name '*.h' \) ! -path '*/ThirdParty/*' -print0 |
        xargs -0 "$clang_format" -i; then
        die "command 'clang-format' failed"
    fi
    passed "C++ formatting ($cpp_file_count files)"

    step "Formatting Rux sources ($manifest_count packages)"
    find Packages Tests -type f -name Rux.toml -print | while IFS= read -r manifest; do
        run_checked rux "$rux_path" --manifest "$manifest" fmt
    done
    passed "Rux formatting ($manifest_count packages)"
fi

elapsed=$(( $(date +%s) - started_at ))
if [ "$check" = true ]; then
    printf '\nFinished format check in %s\n' "$(format_duration "$elapsed")"
else
    printf '\nFinished source formatting in %s\n' "$(format_duration "$elapsed")"
fi
