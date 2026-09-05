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
jobs=
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
        '  --jobs N                       Test/tidy/format workers (default: every CPU); explicit value also limits builds' \
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
        '  sh Run.sh test --skip-build --clang-tidy --jobs 4'
}

require_value() {
    [ "$#" -ge 2 ] || die "option '$1' requires a value"
}

# Options each command accepts, used to reject an option the command ignores.
command_options() {
    case $1 in
    build) printf '%s' '--configuration --build-directory --compiler --jobs' ;;
    test) printf '%s' '--configuration --build-directory --compiler --rux-executable --target --fix-formatting --skip-build --clang-tidy --jobs' ;;
    format) printf '%s' '--rux-executable --check --jobs' ;;
    tidy) printf '%s' '--build-directory --jobs' ;;
    unit) printf '%s' '--configuration --build-directory --jobs' ;;
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
    --jobs)
        require_option "$1"
        require_value "$@"
        jobs=$2
        case "$jobs" in
        *[!0-9]*|'') die "option '--jobs' requires a positive integer" ;;
        esac
        [ "$jobs" -ge 1 ] 2>/dev/null || die "option '--jobs' requires a positive integer"
        shift 2
        ;;
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

worker_count=$jobs
if [ -z "$worker_count" ]; then
    worker_count=$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1')
    [ "$worker_count" -ge 1 ] || worker_count=1
fi

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
    run_checked OutputOwnership sh Tests/Policy/OutputOwnership/Test.sh
    run_checked OutputOwnership sh Tests/Policy/OutputOwnership/Check.sh
}

run_build() {
    command -v cmake >/dev/null 2>&1 || die "required tool 'cmake' was not found; install it and ensure it is available on PATH"
    command -v ninja >/dev/null 2>&1 || die "required tool 'ninja' was not found; install it and ensure it is available on PATH"

    if [ -n "$compiler" ]; then
        compiler_path=$(command -v "$compiler" 2>/dev/null || true)
        [ -n "$compiler_path" ] || die "C++ compiler '$compiler' was not found; install Clang 23 or pass --compiler PATH"
    else
        compiler_path=$(find_llvm_tool clang++ || true)
        [ -n "$compiler_path" ] || die "Clang 23 was not found; install it or pass --compiler PATH"
    fi

    build_started_at=$(date +%s)

    step "Configuring $configuration build"
    run_filtered cmake "$configure_report_program" cmake \
        -S "$repository_root" \
        -B "$build_path" \
        -G Ninja \
        -DCMAKE_BUILD_TYPE="$configuration" \
        -DCMAKE_CXX_COMPILER="$compiler_path" \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DRUX_WERROR=ON \
        -DRUX_BUILD_TESTS=ON

    step "Building compiler and unit tests"
    if [ -n "$jobs" ]; then
        run_filtered cmake "$build_report_program" cmake --build "$build_path" --config "$configuration" --parallel "$jobs"
    else
        run_filtered cmake "$build_report_program" cmake --build "$build_path" --config "$configuration"
    fi

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

# Print the captured output of runs 1..$2 under $1 in order, then fail with
# the first non-zero exit status, the way run_checked would for one command.
flush_concurrent_runs() {
    concurrent_scratch=$1
    concurrent_count=$2
    concurrent_name=$3
    concurrent_status=0
    concurrent_flushed=0
    while [ "$concurrent_flushed" -lt "$concurrent_count" ]; do
        concurrent_flushed=$((concurrent_flushed + 1))
        [ -s "$concurrent_scratch/$concurrent_flushed.out" ] && cat "$concurrent_scratch/$concurrent_flushed.out"
        if [ "$concurrent_status" -eq 0 ] && [ -s "$concurrent_scratch/$concurrent_flushed.failed" ]; then
            concurrent_status=$(cat "$concurrent_scratch/$concurrent_flushed.failed")
        fi
    done
    rm -rf "$concurrent_scratch"
    [ "$concurrent_status" -eq 0 ] && return 0
    printf "error: command '%s' failed with exit code %s\n" "$concurrent_name" "$concurrent_status" >&2
    exit "$concurrent_status"
}

# Run clang-format with the given options over every maintained C++ source,
# in one background batch per worker. Each batch's diagnostics are captured
# and printed whole once every batch has finished, so they never interleave.
run_clang_format() {
    format_scratch=$(mktemp -d "${TMPDIR:-/tmp}/rux-clang-format.XXXXXX")
    format_index=0
    find_maintained_cpp -print | while IFS= read -r source; do
        printf '%s\n' "$source" >>"$format_scratch/list.$((format_index % worker_count + 1))"
        format_index=$((format_index + 1))
    done
    format_batches=0
    for batch in "$format_scratch"/list.*; do
        format_batches=$((format_batches + 1))
        (xargs "$clang_format" "$@" <"$batch" >"$format_scratch/$format_batches.out" 2>&1 ||
            printf '%s' "$?" >"$format_scratch/$format_batches.failed") &
    done
    wait
    flush_concurrent_runs "$format_scratch" "$format_batches" clang-format
}

# Run `rux --manifest <manifest> fmt` with the given options for every package
# and test manifest, worker_count manifests at a time. Each run's report is
# captured and printed in manifest order once every run has finished.
run_rux_fmt() {
    fmt_scratch=$(mktemp -d "${TMPDIR:-/tmp}/rux-fmt.XXXXXX")
    find_manifests >"$fmt_scratch/manifests"
    fmt_index=0
    fmt_running=0
    # Reading from a file rather than a pipe keeps the counters in this shell.
    while IFS= read -r manifest; do
        fmt_index=$((fmt_index + 1))
        ("$rux_path" --manifest "$manifest" fmt "$@" >"$fmt_scratch/$fmt_index.out" 2>&1 ||
            printf '%s' "$?" >"$fmt_scratch/$fmt_index.failed") &
        fmt_running=$((fmt_running + 1))
        if [ "$fmt_running" -ge "$worker_count" ]; then
            wait
            fmt_running=0
        fi
    done <"$fmt_scratch/manifests"
    wait
    flush_concurrent_runs "$fmt_scratch" "$fmt_index" rux
}

# Format maintained C++ and Rux sources, or check them when passed true. Golden
# diagnostic fixtures are intentionally excluded because malformed formatting is
# part of what they test.
run_format() {
    format_check=$1

    clang_format=$(find_llvm_tool clang-format || true)
    [ -n "$clang_format" ] || die "required tool 'clang-format 23' was not found; install it and ensure it is available on PATH"
    resolve_rux

    cpp_file_count=$(find_maintained_cpp -print | wc -l | tr -d '[:space:]')
    manifest_count=$(find_manifests | wc -l | tr -d '[:space:]')

    [ "$cpp_file_count" -gt 0 ] || die "no maintained C++ files were found"
    [ "$manifest_count" -gt 0 ] || die "no Rux package or test manifests were found"

    format_started_at=$(date +%s)
    if [ "$format_check" = true ]; then
        step "Checking C++ formatting ($cpp_file_count files)"
        run_clang_format --dry-run -Werror
        passed "C++ formatting ($cpp_file_count files)"

        step "Checking Rux formatting ($manifest_count packages)"
        run_rux_fmt --check
        passed "Rux formatting ($manifest_count packages)"

        finished "Finished format check in $(format_duration $(($(date +%s) - format_started_at)))"
    else
        step "Formatting C++ sources ($cpp_file_count files)"
        run_clang_format -i
        passed "C++ formatting ($cpp_file_count files)"

        step "Formatting Rux sources ($manifest_count packages)"
        run_rux_fmt
        passed "Rux formatting ($manifest_count packages)"

        finished "Finished source formatting in $(format_duration $(($(date +%s) - format_started_at)))"
    fi
}

# Analyze one translation unit in the background. Its diagnostics are held in a
# per-file capture so the parent can label and print them in submission order.
analyze_tidy_source() {
    if ! "$clang_tidy" --quiet "--config-file=$repository_root/.clang-tidy" \
        -p "$analysis_path" "$1" >"$tidy_scratch/$2.out" 2>&1; then
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
    [ -n "$clang_tidy" ] || die "required tool 'clang-tidy 23' was not found; install it and ensure it is available on PATH"

    analysis_path=$build_path
    if [ -f "$build_path/CMakeCache.txt" ] && grep -q '^RUX_USE_PCH:BOOL=ON$' "$build_path/CMakeCache.txt"; then
        run_checked cmake cmake --build "$build_path" --target rux-analysis-database
        analysis_path=$build_path/Analysis
    fi
    compile_commands=$analysis_path/compile_commands.json
    [ -f "$compile_commands" ] || die "compilation database '$compile_commands' was not found; build the compiler first"

    clang_tidy_jobs=$worker_count

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

# Prelude shared by the report programs below: the compiler's duration and
# count spellings, and the rule that turns the exit-status marker run_filtered
# appends into awk's own exit status, printing whatever partial line preceded it.
report_prelude='
/__rux_status__=[0-9]+$/ {
    marker = index($0, "__rux_status__=")
    status = substr($0, marker + 15) + 0
    if (marker > 1) {
        print substr($0, 1, marker - 1)
        fflush()
    }
    next
}
function duration(seconds,    ms, text) {
    ms = int(seconds * 1000 + 0.5)
    if (ms < 1000) return ms " ms"
    if (ms < 60000) {
        text = sprintf("%.2f", ms / 1000)
        sub(/0+$/, "", text)
        sub(/[.]$/, "", text)
        return text " s"
    }
    return int(ms / 60000) " min " sprintf("%.1f", (ms % 60000) / 1000) " s"
}
function count(n) { return n " " (n == 1 ? "test" : "tests") }
'

# Run a command with its combined output reshaped by an awk report program.
# The command's exit status rides a trailing marker line, which the program
# turns into its own exit status, since POSIX sh has no pipefail; a failure
# is then reported the way run_checked reports one.
run_filtered() {
    filtered_name=$1
    filtered_program=$2
    shift 2
    if {
        "$@" 2>&1
        printf '__rux_status__=%s\n' "$?"
    } | awk -v running_verb="$(status_verb Running 36)" -v passed_verb="$(status_verb Passed 32)" \
        -v failed_verb="$(status_verb Failed 31)" "$report_prelude$filtered_program"; then
        return 0
    else
        filtered_status=$?
    fi
    printf "error: command '%s' failed with exit code %s\n" "$filtered_name" "$filtered_status" >&2
    exit "$filtered_status"
}

# CMake's configure report: `-- Configuring done (0.1s)` becomes
# `Configuring done in 100 ms`, the build-files line becomes a detail, and
# other status lines lose their `-- ` prefix. Anything else passes through.
configure_report_program='
/^-- (Configuring|Generating) done [(][0-9.]+s[)]$/ {
    seconds = $0
    sub(/^.*[(]/, "", seconds)
    sub(/s[)]$/, "", seconds)
    print $2 " done in " duration(seconds)
    fflush()
    next
}
/^-- Build files have been written to: / {
    path = $0
    sub(/^-- Build files have been written to: /, "", path)
    print "  Build files: \047" path "\047"
    fflush()
    next
}
/^-- / { line = $0; sub(/^-- /, "", line); print line; fflush(); next }
{ print; fflush() }
END { exit status }
'

# Ninja progress: `[3/414] Building CXX object .../CMakeFiles/RuxSystem.dir/Process.cpp.obj`
# becomes `Compiling Compiler/System/Process.cpp (3/414)`, a link names its
# output, and an up-to-date tree says so. Diagnostics pass through unchanged.
build_report_program='
/^[[][0-9]+.[0-9]+[]] / {
    progress = $1
    sub(/^[[]/, "(", progress)
    sub(/[]]$/, ")", progress)
    action = $0
    sub(/^[[][0-9]+.[0-9]+[]] /, "", action)
    if (action ~ /^Building CXX object /) {
        source = action
        sub(/^Building CXX object /, "", source)
        gsub("CMakeFiles/[^/]+[.]dir/", "", source)
        sub(/[.](obj|o)$/, "", source)
        gsub("__/", "../", source)
        print "Compiling " source " " progress
    } else if (action ~ /^Linking CXX (executable|static library|shared library) /) {
        target = action
        sub(/^Linking CXX (executable|static library|shared library) /, "", target)
        print "Linking \047" target "\047 " progress
    } else {
        print action " " progress
    }
    fflush()
    next
}
/^ninja: no work to do[.]$/ { print "Up to date"; fflush(); next }
{ print; fflush() }
END { exit status }
'

# Reshapes CTest's report into the `rux test` vocabulary: one line per group as
# it completes, then a total. A failing group's output (--output-on-failure) and
# anything unrecognized pass through unchanged.
unit_report_program='
/^ *[0-9]+.[0-9]+ Test +#[0-9]+: / {
    line = $0
    sub(/^ *[0-9]+./, "", line)
    total = line + 0
    sub(/^[0-9]+ Test +#[0-9]+: /, "", line)
    name = line
    sub(/ .*$/, "", name)
    sub(/^[^ ]+ +[.]* */, "", line)
    seconds = line
    sub(/ sec *$/, "", seconds)
    sub(/^.* /, "", seconds)
    state = line
    sub(/ +[0-9]+[.][0-9]+ sec *$/, "", state)
    sub(/^[*]+/, "", state)
    gsub(/  +/, " ", state)
    sub(/ +$/, "", state)
    if (!announced) { announced = 1; print running_verb " " count(total) }
    detail = name " in " duration(seconds)
    if (state == "Passed") { passed++; print passed_verb " " detail }
    else { failed++; if (state != "Failed") detail = detail " (" state ")"; print failed_verb " " detail }
    fflush()
    next
}
/^Total Test time [(]real[)] += / { elapsed = $(NF - 1); next }
/^[0-9]+% tests passed/ { summarizing = 1; next }
summarizing { next }
/^Test project / { next }
/^ *Start +[0-9]+: / { next }
/^ *$/ { next }
{ print; fflush() }
END {
    if (passed + failed > 0) {
        totals = count(passed + failed) " in " duration(elapsed) " (" passed " passed, " failed " failed)"
        print (status == 0 ? passed_verb : failed_verb) " " totals
    }
    exit status
}
'

run_unit() {
    command -v ctest >/dev/null 2>&1 || die "required tool 'ctest' was not found; install it and ensure it is available on PATH"

    step "Running C++ unit tests"
    run_filtered ctest "$unit_report_program" \
        ctest --test-dir "$build_path" --output-on-failure -C "$configuration" --parallel "$worker_count" --no-tests=error
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
        rux_targeted test --release --jobs "$worker_count"
    else
        rux_targeted test --jobs "$worker_count"
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
