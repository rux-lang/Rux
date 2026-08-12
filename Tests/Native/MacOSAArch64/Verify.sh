#!/bin/sh

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../../.." && pwd)
rux=${1:-"$repo_root/Bin/rux"}
compiler_arch=${RUX_COMPILER_ARCH:-aarch64}

fail() {
    echo "macOS AArch64 fixture failure: $*" >&2
    exit 1
}

[ "$(uname -s)" = "Darwin" ] || fail "the fixtures require macOS"
[ "$(uname -m)" = "arm64" ] || fail "the fixtures require an underlying Apple Silicon host"
[ -f "$rux" ] || fail "compiler '$rux' does not exist"

case "$compiler_arch" in
    aarch64)
        output_dir="$repo_root/Bin/Tests/Native/Release"
        ;;
    x86_64)
        output_dir="$repo_root/Bin/Tests/Native/Release/macos-aarch64"
        ;;
    *)
        fail "unknown compiler architecture '$compiler_arch'"
        ;;
esac

work_dir=$(mktemp -d "${TMPDIR:-/tmp}/rux-macos-aarch64-fixtures.XXXXXX")
trap 'rm -rf "$work_dir"' EXIT HUP INT TERM

run_rux() {
    if [ "$compiler_arch" = "x86_64" ]; then
        /usr/bin/arch -x86_64 "$rux" "$@"
    else
        "$rux" "$@"
    fi
}

build_fixture() {
    fixture=$1
    log_name=$2
    log="$work_dir/$log_name.build.log"
    if ! run_rux --manifest "$script_dir/$fixture/Fixture.toml" build --release --target macos-aarch64 --quiet \
        >"$log" 2>&1; then
        cat "$log" >&2
        fail "failed to build $log_name"
    fi
    if [ -s "$log" ]; then
        cat "$log" >&2
        fail "$log_name produced unexpected build output"
    fi
}

byte_count() {
    od -An -tu1 -j "$2" -N "$3" "$1" 2>/dev/null
}

read_le32() {
    set -- $(byte_count "$1" "$2" 4)
    [ "$#" -eq 4 ] || fail "could not read a 32-bit field from '$1' at offset $2"
    echo $(( $1 | ($2 << 8) | ($3 << 16) | ($4 << 24) ))
}

read_be32() {
    set -- $(byte_count "$1" "$2" 4)
    [ "$#" -eq 4 ] || fail "could not read a big-endian field from '$1' at offset $2"
    echo $(( ($1 << 24) | ($2 << 16) | ($3 << 8) | $4 ))
}

preflight_macho() {
    artifact=$1
    expected_type=$2

    [ -f "$artifact" ] || fail "expected artifact '$artifact' was not produced"
    size=$(wc -c <"$artifact" | tr -d ' ')
    [ "$size" -ge 32 ] || fail "'$artifact' is too small to be a Mach-O image"

    set -- $(byte_count "$artifact" 0 8)
    [ "$#" -eq 8 ] || fail "could not read the Mach-O header from '$artifact'"
    [ "$1 $2 $3 $4" = "207 250 237 254" ] || fail "'$artifact' is not a little-endian Mach-O 64 image"
    [ "$5 $6 $7 $8" = "12 0 0 1" ] || fail "'$artifact' does not declare the ARM64 CPU type"

    file_type=$(read_le32 "$artifact" 12)
    [ "$file_type" -eq "$expected_type" ] || fail "'$artifact' has Mach-O file type $file_type, expected $expected_type"
    command_count=$(read_le32 "$artifact" 16)
    command_bytes=$(read_le32 "$artifact" 20)
    command_end=$((32 + command_bytes))
    [ "$command_end" -le "$size" ] || fail "'$artifact' has load commands beyond the end of the image"

    command_offset=32
    command_index=0
    signature_count=0
    signature_offset=0
    signature_size=0
    while [ "$command_index" -lt "$command_count" ]; do
        [ $((command_offset + 8)) -le "$command_end" ] || fail "'$artifact' has a truncated load command"
        command=$(read_le32 "$artifact" "$command_offset")
        command_size=$(read_le32 "$artifact" $((command_offset + 4)))
        [ "$command_size" -ge 8 ] || fail "'$artifact' has an invalid load-command size"
        [ $((command_offset + command_size)) -le "$command_end" ] || fail "'$artifact' has a load command beyond sizeofcmds"
        if [ "$command" -eq 29 ]; then
            [ "$command_size" -eq 16 ] || fail "'$artifact' has an invalid LC_CODE_SIGNATURE size"
            signature_count=$((signature_count + 1))
            signature_offset=$(read_le32 "$artifact" $((command_offset + 8)))
            signature_size=$(read_le32 "$artifact" $((command_offset + 12)))
        fi
        command_offset=$((command_offset + command_size))
        command_index=$((command_index + 1))
    done

    [ "$command_offset" -eq "$command_end" ] || fail "'$artifact' load commands do not fill sizeofcmds"
    [ "$signature_count" -eq 1 ] || fail "'$artifact' has $signature_count code-signature commands"
    [ "$signature_size" -ge 32 ] || fail "'$artifact' has a truncated embedded signature"
    [ $((signature_offset + signature_size)) -le "$size" ] || fail "'$artifact' has a code signature beyond the image"

    signature_magic=$(read_be32 "$artifact" "$signature_offset")
    [ "$signature_magic" -eq 4208856256 ] || fail "'$artifact' does not contain an embedded-signature superblob"
    superblob_size=$(read_be32 "$artifact" $((signature_offset + 4)))
    [ "$superblob_size" -eq "$signature_size" ] || fail "'$artifact' code-signature sizes disagree"
    slot_count=$(read_be32 "$artifact" $((signature_offset + 8)))
    [ "$slot_count" -eq 1 ] || fail "'$artifact' signature contains $slot_count slots instead of one CodeDirectory"
    slot_type=$(read_be32 "$artifact" $((signature_offset + 12)))
    [ "$slot_type" -eq 0 ] || fail "'$artifact' signature slot is not a CodeDirectory"
    code_directory_offset=$(read_be32 "$artifact" $((signature_offset + 16)))
    [ "$code_directory_offset" -ge 20 ] || fail "'$artifact' has an invalid CodeDirectory offset"
    [ $((code_directory_offset + 8)) -le "$signature_size" ] || fail "'$artifact' CodeDirectory is outside the signature"
    code_directory_magic=$(read_be32 "$artifact" $((signature_offset + code_directory_offset)))
    [ "$code_directory_magic" -eq 4208856066 ] || fail "'$artifact' signature slot is not a CodeDirectory blob"
    code_directory_size=$(read_be32 "$artifact" $((signature_offset + code_directory_offset + 4)))
    [ $((code_directory_offset + code_directory_size)) -le "$signature_size" ] || \
        fail "'$artifact' CodeDirectory extends beyond the signature"
}

run_and_capture() {
    name=$1
    executable=$2
    stdout="$work_dir/$name.stdout"
    stderr="$work_dir/$name.stderr"
    set +e
    # exec leaves no intermediate shell whose signal diagnostic could be mixed
    # into the artifact's own stderr when an assertion deliberately traps.
    (cd "$output_dir" && exec "$executable") >"$stdout" 2>"$stderr"
    run_status=$?
    set -e
}

require_empty_output() {
    name=$1
    [ ! -s "$work_dir/$name.stdout" ] || fail "$name wrote unexpected stdout: $(cat "$work_dir/$name.stdout")"
    [ ! -s "$work_dir/$name.stderr" ] || fail "$name wrote unexpected stderr: $(cat "$work_dir/$name.stderr")"
}

check_trap() {
    name=$1
    prefix=$2
    message=$3

    run_and_capture "$name" "$output_dir/$name"
    [ "$run_status" -eq 133 ] || fail "$name exited with $run_status instead of the SIGTRAP shell status 133"
    [ ! -s "$work_dir/$name.stdout" ] || fail "$name wrote unexpected stdout: $(cat "$work_dir/$name.stdout")"

    sed -E 's# \([^)]*Src[/\\]Main\.rux:[0-9]+:[0-9]+\)$# (Src/Main.rux:<line>:<column>)#' \
        "$work_dir/$name.stderr" >"$work_dir/$name.normalized.stderr"
    printf '%s: %s\n  at Main (Src/Main.rux:<line>:<column>)\n' "$prefix" "$message" >"$work_dir/$name.expected.stderr"
    if ! cmp -s "$work_dir/$name.expected.stderr" "$work_dir/$name.normalized.stderr"; then
        echo "Expected $name stderr:" >&2
        cat "$work_dir/$name.expected.stderr" >&2
        echo "Actual $name stderr:" >&2
        cat "$work_dir/$name.stderr" >&2
        fail "$name produced unexpected diagnostics"
    fi
}

build_fixture ExitCode MacOSAArch64ExitCode
build_fixture LibSystem MacOSAArch64LibSystem
build_fixture Assert MacOSAArch64Assert
build_fixture Panic MacOSAArch64Panic
build_fixture Dylib/Library MacOSAArch64Dylib
build_fixture Dylib/Loader MacOSAArch64DylibLoader

preflight_macho "$output_dir/MacOSAArch64ExitCode" 2
preflight_macho "$output_dir/MacOSAArch64LibSystem" 2
preflight_macho "$output_dir/MacOSAArch64Assert" 2
preflight_macho "$output_dir/MacOSAArch64Panic" 2
preflight_macho "$output_dir/libMacOSAArch64Dylib.dylib" 6
preflight_macho "$output_dir/MacOSAArch64DylibLoader" 2

run_and_capture MacOSAArch64ExitCode "$output_dir/MacOSAArch64ExitCode"
[ "$run_status" -eq 73 ] || fail "MacOSAArch64ExitCode exited with $run_status instead of 73"
require_empty_output MacOSAArch64ExitCode

run_and_capture MacOSAArch64LibSystem "$output_dir/MacOSAArch64LibSystem"
[ "$run_status" -eq 0 ] || fail "MacOSAArch64LibSystem exited with $run_status"
require_empty_output MacOSAArch64LibSystem

check_trap MacOSAArch64Assert "Assertion failed" "native Apple Silicon assertion fixture"
check_trap MacOSAArch64Panic "Panic" "native Apple Silicon panic fixture"

run_and_capture MacOSAArch64DylibLoader "$output_dir/MacOSAArch64DylibLoader"
[ "$run_status" -eq 0 ] || fail "MacOSAArch64DylibLoader exited with $run_status"
require_empty_output MacOSAArch64DylibLoader

echo "macOS AArch64 native runtime fixtures passed ($compiler_arch compiler)"
