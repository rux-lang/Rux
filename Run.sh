#!/bin/sh

# Repository entry point for local development.
# Portable across Linux, macOS, and FreeBSD; ./Run.ps1 is the PowerShell peer.

set -eu

script_directory=$(CDPATH= cd -P "$(dirname "$0")" && pwd)
. "$script_directory/Scripts/RepositoryMessages.sh"

repository_root=$script_directory

subcommand=
configuration=Release
build_directory=Build
compiler=${CXX:-}
rux_executable=
target=
check=false
fix_formatting=false
skip_build=false
clang_tidy_enabled=false

usage() {
    printf '%s\n' \
        'Usage: sh Run.sh <command> [options]' \
        '' \
        'Commands:' \
        '  build     Configure and build the compiler and C++ unit tests' \
        '  test      Run the complete verification workflow' \
        '  format    Format C++ and Rux sources, or check them with --check' \
        '  policy    Run the source-tree policy guards' \
        '  tidy      Run clang-tidy over maintained C++ sources' \
        '  unit      Run the C++ unit tests through CTest' \
        '  clean     Remove the build directory and Bin' \
        '  help      Show this help' \
        '' \
        'Options:' \
        '  --configuration Debug|Release  CMake configuration (build, test, unit; default: Release)' \
        '  --build-directory PATH         CMake build directory (build, test, unit, tidy, clean; default: Build)' \
        '  --compiler PATH                Clang C++ compiler (build, test; default: $CXX or detected Clang)' \
        '  --rux-executable PATH          Existing rux executable (format, test; default: Bin/rux)' \
        '  --target TRIPLE                Check and directly run suites for this target (test; default: the host)' \
        '  --check                        Check formatting without modifying files (format)' \
        '  --fix-formatting               Format sources instead of checking them (test)' \
        '  --skip-build                   Reuse the existing build and executables (test)' \
        '  --clang-tidy                   Add the clang-tidy pass (test)' \
        '  -h, --help                     Show this help' \
        '' \
        'Examples:' \
        '  sh Run.sh build --configuration Debug' \
        '  sh Run.sh format --check' \
        '  sh Run.sh test --skip-build --clang-tidy'
}

require_value() {
    [ "$#" -ge 2 ] || die "option '$1' requires a value"
}

# Options each command accepts, used to reject an option the command ignores.
command_options() {
    case $1 in
    build) printf '%s' '--configuration --build-directory --compiler' ;;
    test) printf '%s' '--configuration --build-directory --compiler --rux-executable --target --fix-formatting --skip-build --clang-tidy' ;;
    format) printf '%s' '--rux-executable --check' ;;
    tidy) printf '%s' '--build-directory' ;;
    unit) printf '%s' '--configuration --build-directory' ;;
    clean) printf '%s' '--build-directory' ;;
    *) printf '%s' '' ;;
    esac
}

require_option() {
    for allowed in $(command_options "$subcommand"); do
        if [ "$allowed" = "$1" ]; then
            return 0
        fi
    done
    die "option '$1' is not valid for command '$subcommand'"
}

if [ "$#" -eq 0 ]; then
    usage
    exit 0
fi

case $1 in
help | -h | --help)
    usage
    exit 0
    ;;
build | test | format | policy | tidy | unit | clean)
    subcommand=$1
    shift
    ;;
*)
    die "unknown command '$1'"
    ;;
esac

while [ "$#" -gt 0 ]; do
    case "$1" in
    --configuration)
        require_option "$1"
        require_value "$@"
        configuration=$2
        shift 2
        ;;
    --build-directory)
        require_option "$1"
        require_value "$@"
        build_directory=$2
        shift 2
        ;;
    --compiler)
        require_option "$1"
        require_value "$@"
        compiler=$2
        shift 2
        ;;
    --rux-executable)
        require_option "$1"
        require_value "$@"
        rux_executable=$2
        shift 2
        ;;
    --target)
        require_option "$1"
        require_value "$@"
        target=$2
        shift 2
        ;;
    --check)
        require_option "$1"
        check=true
        shift
        ;;
    --fix-formatting)
        require_option "$1"
        fix_formatting=true
        shift
        ;;
    --skip-build)
        require_option "$1"
        skip_build=true
        shift
        ;;
    --clang-tidy)
        require_option "$1"
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

case "$build_directory" in
/*) build_path=$build_directory ;;
*) build_path=$repository_root/$build_directory ;;
esac

resolve_rux() {
    if [ -z "$rux_executable" ]; then
        rux_path=$repository_root/Bin/rux
    else
        case "$rux_executable" in
        /*) rux_path=$rux_executable ;;
        *) rux_path=$repository_root/$rux_executable ;;
        esac
    fi

    [ -f "$rux_path" ] || die "rux executable '$rux_path' was not found; build it first or pass --rux-executable"
}

# Run a target-aware rux subcommand for the selected target. `--target` follows the
# subcommand, while the global `--manifest` precedes it, so the option is
# appended rather than inserted. `lint` takes no target and is invoked directly.
rux_targeted() {
    if [ -n "$target" ]; then
        run_checked rux "$rux_path" "$@" --target "$target"
    else
        run_checked rux "$rux_path" "$@"
    fi
}

# Maintained translation units in the compilation database: Compiler/ and
# Tests/Unit/ without vendored third-party sources.
list_tidy_sources() {
    sed -n 's/.*"file"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$1" |
        grep -E '/(Compiler|Tests/Unit)/' |
        grep -v ThirdParty |
        sort -u
}

relative_to_root() {
    case "$1" in
    "$repository_root"/*) printf '%s' "${1#"$repository_root"/}" ;;
    *) printf '%s' "$1" ;;
    esac
}

run_policy() {
    step "Checking source-tree policy"
    run_checked PlatformIsolation sh Tests/Policy/PlatformIsolation/Check.sh
    run_checked NoExternalToolchain sh Tests/Policy/NoExternalToolchain/Check.sh
    run_checked LanguageCutover sh Tests/Policy/LanguageCutover/Test.sh
    run_checked LanguageCutover sh Tests/Policy/LanguageCutover/Check.sh
    run_checked EnumVariantCutover sh Tests/Policy/EnumVariantCutover/Test.sh
    run_checked EnumVariantCutover sh Tests/Policy/EnumVariantCutover/Check.sh
    run_checked OversizedFiles sh Tests/Policy/OversizedFiles/Test.sh
    run_checked OversizedFiles sh Tests/Policy/OversizedFiles/Check.sh
    run_checked ScriptMessages sh Tests/Policy/ScriptMessages/Check.sh
    run_checked InstallerMessages sh Tests/Policy/InstallerMessages/Check.sh
    run_checked UserMessages sh Tests/Policy/UserMessages/Test.sh
    run_checked UserMessages sh Tests/Policy/UserMessages/Check.sh
}

run_build() {
    command -v cmake >/dev/null 2>&1 || die "required tool 'cmake' was not found; install it and ensure it is available on PATH"
    command -v ninja >/dev/null 2>&1 || die "required tool 'ninja' was not found; install it and ensure it is available on PATH"

    if [ -n "$compiler" ]; then
        compiler_path=$(command -v "$compiler" 2>/dev/null || true)
        [ -n "$compiler_path" ] || die "C++ compiler '$compiler' was not found; install Clang 22 or pass --compiler PATH"
    else
        compiler_path=$(find_llvm_tool clang++ || true)
        [ -n "$compiler_path" ] || die "Clang 22 was not found; install it or pass --compiler PATH"
    fi

    build_started_at=$(date +%s)

    step "Configuring $configuration build"
    run_checked cmake cmake \
        -S "$repository_root" \
        -B "$build_path" \
        -G Ninja \
        -DCMAKE_BUILD_TYPE="$configuration" \
        -DCMAKE_CXX_COMPILER="$compiler_path" \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DRUX_WERROR=ON \
        -DRUX_BUILD_TESTS=ON

    step "Building compiler and unit tests"
    run_checked cmake cmake --build "$build_path" --config "$configuration"

    built_rux=$repository_root/Bin/rux
    [ -f "$built_rux" ] || die "build completed without producing the expected compiler at '$built_rux'"

    finished "Finished build in $(format_duration $(($(date +%s) - build_started_at)))"
    printf "  Output: '%s'\n" "$built_rux"
}

# Maintained C++ sources: Compiler/ and Tests/Unit/ without vendored code. The
# caller supplies the find action, so one expression serves counting and running.
find_maintained_cpp() {
    find Compiler Tests/Unit -type f \( -name '*.cpp' -o -name '*.h' \) ! -path '*/ThirdParty/*' "$@"
}

find_manifests() {
    find Packages Tests -type f -name Rux.toml -print
}

# Format maintained C++ and Rux sources, or check them when passed true. Golden
# diagnostic fixtures are intentionally excluded because malformed formatting is
# part of what they test.
run_format() {
    format_check=$1

    clang_format=$(find_llvm_tool clang-format || true)
    [ -n "$clang_format" ] || die "required tool 'clang-format 22' was not found; install it and ensure it is available on PATH"
    resolve_rux

    cpp_file_count=$(find_maintained_cpp -print | wc -l | tr -d '[:space:]')
    manifest_count=$(find_manifests | wc -l | tr -d '[:space:]')

    [ "$cpp_file_count" -gt 0 ] || die "no maintained C++ files were found"
    [ "$manifest_count" -gt 0 ] || die "no Rux package or test manifests were found"

    format_started_at=$(date +%s)
    if [ "$format_check" = true ]; then
        step "Checking C++ formatting ($cpp_file_count files)"
        if ! find_maintained_cpp -print0 | xargs -0 "$clang_format" --dry-run -Werror; then
            die "command 'clang-format' failed"
        fi
        passed "C++ formatting ($cpp_file_count files)"

        step "Checking Rux formatting ($manifest_count packages)"
        find_manifests | while IFS= read -r manifest; do
            run_checked rux "$rux_path" --manifest "$manifest" fmt --check
        done
        passed "Rux formatting ($manifest_count packages)"

        finished "Finished format check in $(format_duration $(($(date +%s) - format_started_at)))"
    else
        step "Formatting C++ sources ($cpp_file_count files)"
        if ! find_maintained_cpp -print0 | xargs -0 "$clang_format" -i; then
            die "command 'clang-format' failed"
        fi
        passed "C++ formatting ($cpp_file_count files)"

        step "Formatting Rux sources ($manifest_count packages)"
        find_manifests | while IFS= read -r manifest; do
            run_checked rux "$rux_path" --manifest "$manifest" fmt
        done
        passed "Rux formatting ($manifest_count packages)"

        finished "Finished source formatting in $(format_duration $(($(date +%s) - format_started_at)))"
    fi
}

# Analyze one translation unit in the background. Its diagnostics are held in a
# per-file capture so the parent can label and print them in submission order.
analyze_tidy_source() {
    if ! "$clang_tidy" --quiet "--config-file=$repository_root/.clang-tidy" \
        -p "$build_path" "$1" >"$tidy_scratch/$2.out" 2>&1; then
        : >"$tidy_scratch/$2.failed"
    fi
}

# Report every source submitted since the last flush, oldest first.
flush_tidy_results() {
    while [ "$tidy_flushed" -lt "$tidy_index" ]; do
        tidy_flushed=$((tidy_flushed + 1))
        tidy_label="$(relative_to_root "$(sed -n "${tidy_flushed}p" "$tidy_scratch/sources")") ($tidy_flushed/$tidy_total)"
        if [ -f "$tidy_scratch/$tidy_flushed.failed" ]; then
            tidy_failures=$((tidy_failures + 1))
            failed "$tidy_label"
        else
            passed "$tidy_label"
        fi
        if [ -s "$tidy_scratch/$tidy_flushed.out" ]; then
            cat "$tidy_scratch/$tidy_flushed.out"
        fi
    done
}

run_tidy() {
    clang_tidy=$(find_llvm_tool clang-tidy || true)
    [ -n "$clang_tidy" ] || die "required tool 'clang-tidy 22' was not found; install it and ensure it is available on PATH"

    compile_commands=$build_path/compile_commands.json
    [ -f "$compile_commands" ] || die "compilation database '$compile_commands' was not found; build the compiler first"

    clang_tidy_jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1')
    [ "$clang_tidy_jobs" -le 4 ] || clang_tidy_jobs=4

    tidy_scratch=$(mktemp -d "${TMPDIR:-/tmp}/rux-clang-tidy.XXXXXX")
    trap 'rm -rf "$tidy_scratch"' EXIT HUP INT TERM
    list_tidy_sources "$compile_commands" >"$tidy_scratch/sources"
    tidy_total=$(wc -l <"$tidy_scratch/sources" | tr -d '[:space:]')
    [ "$tidy_total" -gt 0 ] || die "no maintained C++ translation units were found in '$compile_commands'"

    step "Running clang-tidy ($tidy_total files)"
    clang_tidy_started_at=$(date +%s)
    tidy_index=0
    tidy_flushed=0
    tidy_failures=0
    tidy_running=0
    # Sources are submitted in batches because POSIX `wait` cannot reap one job
    # at a time; a batch boundary also gives the flush a stable report order.
    # Reading from a file rather than a pipe keeps the counters in this shell.
    while IFS= read -r tidy_source; do
        tidy_index=$((tidy_index + 1))
        analyze_tidy_source "$tidy_source" "$tidy_index" &
        tidy_running=$((tidy_running + 1))
        if [ "$tidy_running" -ge "$clang_tidy_jobs" ]; then
            wait
            tidy_running=0
            flush_tidy_results
        fi
    done <"$tidy_scratch/sources"
    wait
    flush_tidy_results

    clang_tidy_elapsed=$(format_duration $(($(date +%s) - clang_tidy_started_at)))
    if [ "$tidy_failures" -ne 0 ]; then
        failed "clang-tidy ($tidy_failures/$tidy_total files failed) in $clang_tidy_elapsed"
        die "clang-tidy failed for $tidy_failures files"
    fi
    passed "clang-tidy ($tidy_total files) in $clang_tidy_elapsed"
}

run_unit() {
    command -v ctest >/dev/null 2>&1 || die "required tool 'ctest' was not found; install it and ensure it is available on PATH"

    step "Running C++ unit tests"
    run_checked ctest ctest --test-dir "$build_path" --output-on-failure -C "$configuration"
}

run_rux_suites() {
    resolve_rux
    workspace_manifest=$repository_root/Rux.toml
    [ -f "$workspace_manifest" ] || die "workspace manifest '$workspace_manifest' was not found"

    step "Checking all Rux workspace packages"
    rux_targeted --manifest "$workspace_manifest" check

    step "Linting all Rux workspace packages"
    run_checked rux "$rux_path" --manifest "$workspace_manifest" lint

    step "Running all Rux test packages"
    if [ "$configuration" = Release ]; then
        rux_targeted test --release
    else
        rux_targeted test
    fi
}

run_clean() {
    step "Removing build outputs"
    for path in "$build_path" "$repository_root/Bin"; do
        if [ -e "$path" ]; then
            rm -rf "$path"
            printf "  Removed '%s'\n" "$path"
        else
            printf "  Skipped '%s' (not present)\n" "$path"
        fi
    done
}

run_test() {
    run_policy

    if [ "$skip_build" = false ]; then
        run_build
    else
        step "Skipping compiler build"
        printf "  note: using the existing build in '%s'\n" "$build_path"
    fi

    if [ "$fix_formatting" = true ]; then
        run_format false
    else
        run_format true
    fi

    if [ "$clang_tidy_enabled" = true ]; then
        run_tidy
    fi

    run_unit
    run_rux_suites
}

started_at=$(date +%s)
cd "$repository_root"

case $subcommand in
build)
    run_build
    ;;
format)
    if [ "$check" = true ]; then
        run_format true
    else
        run_format false
    fi
    ;;
policy)
    run_policy
    finished "Finished policy checks in $(format_duration $(($(date +%s) - started_at)))"
    ;;
tidy)
    run_tidy
    ;;
unit)
    run_unit
    finished "Finished unit tests in $(format_duration $(($(date +%s) - started_at)))"
    ;;
clean)
    run_clean
    finished "Finished clean in $(format_duration $(($(date +%s) - started_at)))"
    ;;
test)
    run_test
    finished "Finished test workflow in $(format_duration $(($(date +%s) - started_at)))"
    ;;
esac
