#!/bin/sh

# Configure and build the Rux compiler and C++ unit-test target.
# Portable across Linux, macOS, and FreeBSD; requires CMake, Ninja, and Clang.

set -eu

script_directory=$(CDPATH= cd -P "$(dirname "$0")" && pwd)
. "$script_directory/Scripts/RepositoryMessages.sh"

configuration=Release
build_directory=Build
compiler=${CXX:-}

usage() {
    printf '%s\n' \
        'Usage: sh Build.sh [options]' \
        '' \
        'Options:' \
        '  --configuration Debug|Release  CMake configuration (default: Release)' \
        '  --build-directory PATH         CMake build directory (default: Build)' \
        '  --compiler PATH                Clang C++ compiler (default: $CXX or detected Clang)' \
        '  -h, --help                     Show this help'
}

require_value() {
    [ "$#" -ge 2 ] || die "option '$1' requires a value"
}

while [ "$#" -gt 0 ]; do
    case "$1" in
    --configuration)
        require_value "$@"
        configuration=$2
        shift 2
        ;;
    --build-directory)
        require_value "$@"
        build_directory=$2
        shift 2
        ;;
    --compiler)
        require_value "$@"
        compiler=$2
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

case "$configuration" in
Debug | Release) ;;
*) die "configuration must be Debug or Release" ;;
esac

repository_root=$script_directory

case "$build_directory" in
/*) build_path=$build_directory ;;
*) build_path=$repository_root/$build_directory ;;
esac

command -v cmake >/dev/null 2>&1 || die "required tool 'cmake' was not found; install it and ensure it is available on PATH"
command -v ninja >/dev/null 2>&1 || die "required tool 'ninja' was not found; install it and ensure it is available on PATH"

if [ -n "$compiler" ]; then
    compiler_path=$(command -v "$compiler" 2>/dev/null || true)
    [ -n "$compiler_path" ] || die "C++ compiler '$compiler' was not found; install Clang 22 or pass --compiler PATH"
else
    for candidate in clang++-22 clang++22 \
        /opt/homebrew/opt/llvm@22/bin/clang++ /usr/local/opt/llvm@22/bin/clang++ \
        /opt/homebrew/opt/llvm/bin/clang++ /usr/local/opt/llvm/bin/clang++ clang++; do
        if compiler_path=$(command -v "$candidate" 2>/dev/null); then
            break
        fi
        compiler_path=
    done
    [ -n "$compiler_path" ] || die "Clang 22 was not found; install it or pass --compiler PATH"
fi

started_at=$(date +%s)

printf '\n==> Configuring %s build\n' "$configuration"
run_checked cmake cmake \
    -S "$repository_root" \
    -B "$build_path" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE="$configuration" \
    -DCMAKE_CXX_COMPILER="$compiler_path" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DRUX_WERROR=ON \
    -DRUX_BUILD_TESTS=ON

printf '\n==> Building compiler and unit tests\n'
run_checked cmake cmake --build "$build_path" --config "$configuration"

rux_executable=$repository_root/Bin/rux
[ -f "$rux_executable" ] || die "build completed without producing '$rux_executable'"

elapsed=$(( $(date +%s) - started_at ))
printf '\nFinished build in %s\n' "$(format_duration "$elapsed")"
printf "  Output: '%s'\n" "$rux_executable"
