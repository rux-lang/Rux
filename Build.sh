#!/bin/sh

# Configure and build the Rux compiler and C++ unit-test target.
# Portable across Linux, macOS, and FreeBSD; requires CMake, Ninja, and Clang.

set -eu

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
        '  --compiler PATH                C++ compiler (default: $CXX or detected Clang)' \
        '  -h, --help                     Show this help'
}

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
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

script_directory=$(CDPATH= cd -P "$(dirname "$0")" && pwd)
repository_root=$script_directory

case "$build_directory" in
/*) build_path=$build_directory ;;
*) build_path=$repository_root/$build_directory ;;
esac

command -v cmake >/dev/null 2>&1 || die "required tool not found: cmake"
command -v ninja >/dev/null 2>&1 || die "required tool not found: ninja"

if [ -n "$compiler" ]; then
    compiler_path=$(command -v "$compiler" 2>/dev/null || true)
    [ -n "$compiler_path" ] || die "C++ compiler not found: $compiler"
else
    for candidate in clang++-22 clang++22 \
        /opt/homebrew/opt/llvm@22/bin/clang++ /usr/local/opt/llvm@22/bin/clang++ \
        /opt/homebrew/opt/llvm/bin/clang++ /usr/local/opt/llvm/bin/clang++ clang++; do
        if compiler_path=$(command -v "$candidate" 2>/dev/null); then
            break
        fi
        compiler_path=
    done
    [ -n "$compiler_path" ] || die "Clang not found; install Clang 22 or set CXX"
fi

started_at=$(date +%s)

printf '\n==> Configuring %s build\n' "$configuration"
cmake \
    -S "$repository_root" \
    -B "$build_path" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE="$configuration" \
    -DCMAKE_CXX_COMPILER="$compiler_path" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DRUX_WERROR=ON \
    -DRUX_BUILD_TESTS=ON

printf '\n==> Building compiler and unit tests\n'
cmake --build "$build_path" --config "$configuration"

rux_executable=$repository_root/Bin/rux
[ -f "$rux_executable" ] || die "build completed without producing '$rux_executable'"

elapsed=$(( $(date +%s) - started_at ))
printf '\nBuild passed in %02d:%02d: %s\n' "$((elapsed / 60))" "$((elapsed % 60))" "$rux_executable"
