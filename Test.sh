#!/bin/sh

# Build and run the complete repository verification workflow.
# Portable across Linux, macOS, and FreeBSD.

set -eu

configuration=Release
build_directory=Build
compiler=${CXX:-}
rux_executable=
skip_build=false
fix_formatting=false
clang_tidy_enabled=false

usage() {
    printf '%s\n' \
        'Usage: sh Test.sh [options]' \
        '' \
        'Options:' \
        '  --configuration Debug|Release  CMake configuration (default: Release)' \
        '  --build-directory PATH         CMake build directory (default: Build)' \
        '  --compiler PATH                C++ compiler passed to Build.sh (default: $CXX or detected Clang)' \
        '  --rux-executable PATH          Existing rux executable (default: Bin/rux)' \
        '  --skip-build                   Reuse the existing build and executables' \
        '  --fix-formatting               Format C++ and Rux sources instead of checking them' \
        '  --clang-tidy                   Run clang-tidy over maintained C++ sources' \
        '  -h, --help                     Show this help'
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
    --rux-executable)
        require_value "$@"
        rux_executable=$2
        shift 2
        ;;
    --skip-build)
        skip_build=true
        shift
        ;;
    --fix-formatting)
        fix_formatting=true
        shift
        ;;
    --clang-tidy)
        clang_tidy_enabled=true
        shift
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

if [ -z "$rux_executable" ]; then
    rux_path=$repository_root/Bin/rux
else
    case "$rux_executable" in
    /*) rux_path=$rux_executable ;;
    *) rux_path=$repository_root/$rux_executable ;;
    esac
fi

command -v ctest >/dev/null 2>&1 || die "required tool not found: ctest"

if [ "$clang_tidy_enabled" = true ]; then
    clang_tidy=
    for candidate in clang-tidy-22 clang-tidy22 \
        /opt/homebrew/opt/llvm@22/bin/clang-tidy /usr/local/opt/llvm@22/bin/clang-tidy \
        /opt/homebrew/opt/llvm/bin/clang-tidy /usr/local/opt/llvm/bin/clang-tidy clang-tidy; do
        if clang_tidy=$(command -v "$candidate" 2>/dev/null); then
            break
        fi
        clang_tidy=
    done
    [ -n "$clang_tidy" ] || die "required tool not found: clang-tidy 22"

    run_clang_tidy=
    for candidate in run-clang-tidy-22 run-clang-tidy22 \
        /opt/homebrew/opt/llvm@22/bin/run-clang-tidy /usr/local/opt/llvm@22/bin/run-clang-tidy \
        /opt/homebrew/opt/llvm/bin/run-clang-tidy /usr/local/opt/llvm/bin/run-clang-tidy run-clang-tidy; do
        if run_clang_tidy=$(command -v "$candidate" 2>/dev/null); then
            break
        fi
        run_clang_tidy=
    done
    [ -n "$run_clang_tidy" ] || die "required tool not found: run-clang-tidy"
fi

started_at=$(date +%s)
cd "$repository_root"

step "Checking platform isolation"
sh Tests/Policy/PlatformIsolation/Check.sh

if [ "$skip_build" = false ]; then
    if [ -n "$compiler" ]; then
        sh "$repository_root/Build.sh" \
            --configuration "$configuration" \
            --build-directory "$build_directory" \
            --compiler "$compiler"
    else
        sh "$repository_root/Build.sh" \
            --configuration "$configuration" \
            --build-directory "$build_directory"
    fi
fi

[ -f "$rux_path" ] || die "Rux executable not found at '$rux_path'"

if [ "$fix_formatting" = true ]; then
    sh "$repository_root/Format.sh" --rux-executable "$rux_path"
else
    sh "$repository_root/Format.sh" --check --rux-executable "$rux_path"
fi

if [ "$clang_tidy_enabled" = true ]; then
    compile_commands=$build_path/compile_commands.json
    [ -f "$compile_commands" ] || die "compilation database not found at '$compile_commands'; run Test.sh without --skip-build first"

    step "Running clang-tidy"
    clang_tidy_jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1')
    [ "$clang_tidy_jobs" -le 4 ] || clang_tidy_jobs=4
    clang_tidy_files=$(grep -c '"file":' "$compile_commands")
    clang_tidy_started_at=$(date +%s)
    if "$run_clang_tidy" -quiet -j "$clang_tidy_jobs" \
        -clang-tidy-binary "$clang_tidy" \
        -config-file "$repository_root/.clang-tidy" \
        -p "$build_path"; then
        clang_tidy_elapsed=$(( $(date +%s) - clang_tidy_started_at ))
        if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
            printf '\033[32m[PASSED]\033[0m clang-tidy (%s files, %ss)\n' "$clang_tidy_files" "$clang_tidy_elapsed"
        else
            printf '[PASSED] clang-tidy (%s files, %ss)\n' "$clang_tidy_files" "$clang_tidy_elapsed"
        fi
    else
        clang_tidy_elapsed=$(( $(date +%s) - clang_tidy_started_at ))
        if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
            printf '\033[31m[FAILED]\033[0m clang-tidy (%ss)\n' "$clang_tidy_elapsed"
        else
            printf '[FAILED] clang-tidy (%ss)\n' "$clang_tidy_elapsed"
        fi
        exit 1
    fi
fi

step "Running C++ unit tests"
ctest --test-dir "$build_path" --output-on-failure -C "$configuration"

workspace_manifest=$repository_root/Rux.toml
[ -f "$workspace_manifest" ] || die "workspace manifest not found at '$workspace_manifest'"

step "Checking all Rux workspace packages"
"$rux_path" --manifest "$workspace_manifest" check

step "Linting all Rux workspace packages"
"$rux_path" --manifest "$workspace_manifest" lint

step "Running all Rux test packages"
if [ "$configuration" = Release ]; then
    "$rux_path" test --release
else
    "$rux_path" test
fi

elapsed=$(( $(date +%s) - started_at ))
printf '\nTest workflow passed in %02d:%02d.\n' "$((elapsed / 60))" "$((elapsed % 60))"
