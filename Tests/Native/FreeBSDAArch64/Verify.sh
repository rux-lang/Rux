#!/bin/sh

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../../.." && pwd)
rux=${1:-"$repo_root/Bin/rux"}
output_dir="$repo_root/Bin/Tests/Native/Release/FreeBSD/AArch64"
syscall_output="$repo_root/Bin/Tests/Packages/FreeBSD/Release/FreeBSD/AArch64/Syscall"

fail() {
    echo "FreeBSD AArch64 fixture failure: $*" >&2
    exit 1
}

[ "$(uname -s)" = "FreeBSD" ] || fail "the fixtures require FreeBSD"
case "$(uname -m)" in
    arm64 | aarch64) ;;
    *) fail "the fixtures require a native AArch64 kernel" ;;
esac
[ -f "$rux" ] || fail "compiler '$rux' does not exist"

work_dir=$(mktemp -d "${TMPDIR:-/tmp}/rux-freebsd-aarch64-fixtures.XXXXXX")
trap 'rm -rf "$work_dir"' EXIT HUP INT TERM

build_fixture() {
    fixture=$1
    name=$2
    log="$work_dir/$name.build.log"
    if ! "$rux" --manifest "$script_dir/$fixture/Fixture.toml" build --release \
        --target freebsd-aarch64 --quiet >"$log" 2>&1; then
        cat "$log" >&2
        fail "generation failed while building $name"
    fi
    if [ -s "$log" ]; then
        cat "$log" >&2
        fail "$name produced unexpected build output"
    fi
}

build_syscall_fixture() {
    log="$work_dir/FreeBSDAArch64Syscall.build.log"
    if ! "$rux" --manifest "$repo_root/Tests/Packages/FreeBSD/Syscall/Rux.toml" build --release \
        --target freebsd-aarch64 --quiet >"$log" 2>&1; then
        cat "$log" >&2
        fail "generation failed while building the BSD syscall package test"
    fi
    if [ -s "$log" ]; then
        cat "$log" >&2
        fail "the BSD syscall package test produced unexpected build output"
    fi
}

byte_count() {
    od -An -tu1 -j "$2" -N "$3" "$1" 2>/dev/null
}

read_le16() {
    set -- $(byte_count "$1" "$2" 2)
    [ "$#" -eq 2 ] || fail "could not read a 16-bit ELF field from '$1' at offset $2"
    echo $(( $1 | ($2 << 8) ))
}

read_le32() {
    set -- $(byte_count "$1" "$2" 4)
    [ "$#" -eq 4 ] || fail "could not read a 32-bit ELF field from '$1' at offset $2"
    echo $(( $1 | ($2 << 8) | ($3 << 16) | ($4 << 24) ))
}

read_le64() {
    set -- $(byte_count "$1" "$2" 8)
    [ "$#" -eq 8 ] || fail "could not read a 64-bit ELF field from '$1' at offset $2"
    echo $(( $1 | ($2 << 8) | ($3 << 16) | ($4 << 24) | \
        ($5 << 32) | ($6 << 40) | ($7 << 48) | ($8 << 56) ))
}

# Repository-owned ELF reader for native preflight. It deliberately uses only
# POSIX utilities and byte offsets from the ELF64 layout, never host ELF tools.
preflight_elf() {
    artifact=$1
    expected_type=$2
    dynamic_policy=$3
    interpreter_policy=$4

    [ -f "$artifact" ] || fail "generation did not produce '$artifact'"
    size=$(wc -c <"$artifact" | tr -d ' ')
    [ "$size" -ge 64 ] || fail "ELF structure: '$artifact' is smaller than an ELF64 header"

    set -- $(byte_count "$artifact" 0 16)
    [ "$#" -eq 16 ] || fail "ELF structure: could not read the identity from '$artifact'"
    [ "$1 $2 $3 $4" = "127 69 76 70" ] || fail "ELF structure: '$artifact' has no ELF magic"
    [ "$5 $6 $7" = "2 1 1" ] || fail "ELF structure: '$artifact' is not little-endian ELF64"
    [ "$8" -eq 9 ] || fail "ELF structure: '$artifact' has OSABI $8 instead of FreeBSD 9"

    file_type=$(read_le16 "$artifact" 16)
    machine=$(read_le16 "$artifact" 18)
    [ "$file_type" -eq "$expected_type" ] || \
        fail "ELF structure: '$artifact' has type $file_type, expected $expected_type"
    [ "$machine" -eq 183 ] || fail "ELF structure: '$artifact' has machine $machine instead of AArch64"

    entry=$(read_le64 "$artifact" 24)
    ph_offset=$(read_le64 "$artifact" 32)
    ph_size=$(read_le16 "$artifact" 54)
    ph_count=$(read_le16 "$artifact" 56)
    [ "$ph_size" -eq 56 ] || fail "ELF structure: '$artifact' has program-header size $ph_size"
    [ "$ph_count" -gt 0 ] || fail "ELF structure: '$artifact' has no program headers"
    [ $((ph_offset + ph_size * ph_count)) -le "$size" ] || \
        fail "ELF structure: '$artifact' has program headers beyond the file"

    index=0
    load_count=0
    entry_segment_count=0
    dynamic_count=0
    interpreter_count=0
    while [ "$index" -lt "$ph_count" ]; do
        offset=$((ph_offset + index * ph_size))
        segment_type=$(read_le32 "$artifact" "$offset")
        segment_flags=$(read_le32 "$artifact" $((offset + 4)))
        file_offset=$(read_le64 "$artifact" $((offset + 8)))
        virtual_address=$(read_le64 "$artifact" $((offset + 16)))
        file_size=$(read_le64 "$artifact" $((offset + 32)))
        memory_size=$(read_le64 "$artifact" $((offset + 40)))
        alignment=$(read_le64 "$artifact" $((offset + 48)))

        [ "$file_size" -le "$memory_size" ] || \
            fail "ELF structure: '$artifact' segment $index has filesz greater than memsz"
        [ $((file_offset + file_size)) -le "$size" ] || \
            fail "ELF structure: '$artifact' segment $index extends beyond the file"
        if [ "$alignment" -gt 1 ]; then
            [ $((virtual_address % alignment)) -eq $((file_offset % alignment)) ] || \
                fail "ELF structure: '$artifact' segment $index violates load congruence"
        fi

        case "$segment_type" in
            1)
                load_count=$((load_count + 1))
                if [ $((segment_flags & 1)) -ne 0 ] && [ "$entry" -ge "$virtual_address" ] && \
                    [ "$entry" -lt $((virtual_address + memory_size)) ]; then
                    entry_segment_count=$((entry_segment_count + 1))
                fi
                ;;
            2)
                dynamic_count=$((dynamic_count + 1))
                ;;
            3)
                interpreter_count=$((interpreter_count + 1))
                interpreter=$(dd if="$artifact" bs=1 skip="$file_offset" count="$file_size" 2>/dev/null | tr -d '\000')
                [ "$interpreter" = "/libexec/ld-elf.so.1" ] || \
                    fail "ELF structure: '$artifact' requests unexpected interpreter '$interpreter'"
                ;;
        esac
        index=$((index + 1))
    done

    [ "$load_count" -gt 0 ] || fail "ELF structure: '$artifact' has no load segments"
    if [ "$expected_type" -eq 2 ]; then
        [ "$entry" -ne 0 ] || fail "ELF structure: executable '$artifact' has no entry point"
        [ "$entry_segment_count" -eq 1 ] || \
            fail "ELF structure: '$artifact' entry is not in exactly one executable load segment"
    else
        [ "$entry" -eq 0 ] || fail "ELF structure: shared library '$artifact' has an entry point"
    fi

    case "$dynamic_policy" in
        required) [ "$dynamic_count" -eq 1 ] || fail "ELF structure: '$artifact' has no unique PT_DYNAMIC" ;;
        forbidden) [ "$dynamic_count" -eq 0 ] || fail "ELF structure: '$artifact' unexpectedly has PT_DYNAMIC" ;;
    esac
    case "$interpreter_policy" in
        required) [ "$interpreter_count" -eq 1 ] || fail "ELF structure: '$artifact' has no unique PT_INTERP" ;;
        forbidden) [ "$interpreter_count" -eq 0 ] || fail "ELF structure: '$artifact' unexpectedly has PT_INTERP" ;;
    esac
}

run_and_capture() {
    name=$1
    executable=$2
    stdout="$work_dir/$name.stdout"
    stderr="$work_dir/$name.stderr"
    set +e
    (cd "$output_dir" && exec "$executable") >"$stdout" 2>"$stderr"
    run_status=$?
    set -e
}

require_empty_output() {
    name=$1
    [ ! -s "$work_dir/$name.stdout" ] || fail "runtime: $name wrote unexpected stdout: $(cat "$work_dir/$name.stdout")"
    [ ! -s "$work_dir/$name.stderr" ] || fail "runtime: $name wrote unexpected stderr: $(cat "$work_dir/$name.stderr")"
}

check_trap() {
    name=$1
    prefix=$2
    message=$3

    run_and_capture "$name" "$output_dir/$name"
    [ "$run_status" -ne 0 ] || fail "runtime: $name unexpectedly succeeded"
    [ ! -s "$work_dir/$name.stdout" ] || fail "runtime: $name wrote unexpected stdout"

    sed -E 's# \([^)]*Src[/\\]Main\.rux:[0-9]+:[0-9]+\)$# (Src/Main.rux:<line>:<column>)#' \
        "$work_dir/$name.stderr" >"$work_dir/$name.normalized.stderr"
    printf '%s: %s\n  at Main (Src/Main.rux:<line>:<column>)\n' "$prefix" "$message" \
        >"$work_dir/$name.expected.stderr"
    if ! cmp -s "$work_dir/$name.expected.stderr" "$work_dir/$name.normalized.stderr"; then
        echo "Expected $name stderr:" >&2
        cat "$work_dir/$name.expected.stderr" >&2
        echo "Actual $name stderr:" >&2
        cat "$work_dir/$name.stderr" >&2
        fail "runtime: $name produced unexpected diagnostics"
    fi
}

build_fixture ExitCode FreeBSDAArch64ExitCode
build_fixture LibC FreeBSDAArch64LibC
build_fixture Assert FreeBSDAArch64Assert
build_fixture Panic FreeBSDAArch64Panic
build_fixture Shared/Library FreeBSDAArch64Fixture
build_fixture Shared/Loader FreeBSDAArch64Loader
build_syscall_fixture

preflight_elf "$output_dir/FreeBSDAArch64ExitCode" 2 forbidden forbidden
preflight_elf "$output_dir/FreeBSDAArch64LibC" 2 required required
preflight_elf "$output_dir/FreeBSDAArch64Assert" 2 forbidden forbidden
preflight_elf "$output_dir/FreeBSDAArch64Panic" 2 forbidden forbidden
preflight_elf "$output_dir/libFreeBSDAArch64Fixture.so" 3 required forbidden
preflight_elf "$output_dir/FreeBSDAArch64Loader" 2 required required
preflight_elf "$syscall_output" 2 forbidden forbidden

run_and_capture FreeBSDAArch64ExitCode "$output_dir/FreeBSDAArch64ExitCode"
[ "$run_status" -eq 73 ] || fail "runtime: FreeBSDAArch64ExitCode exited with $run_status instead of 73"
require_empty_output FreeBSDAArch64ExitCode

run_and_capture FreeBSDAArch64LibC "$output_dir/FreeBSDAArch64LibC"
[ "$run_status" -eq 0 ] || fail "runtime: FreeBSDAArch64LibC exited with $run_status"
printf 'freebsd-aarch64:42:2.5\n' >"$work_dir/FreeBSDAArch64LibC.expected.stdout"
cmp -s "$work_dir/FreeBSDAArch64LibC.expected.stdout" "$work_dir/FreeBSDAArch64LibC.stdout" || \
    fail "runtime: FreeBSDAArch64LibC produced unexpected stdout"
[ ! -s "$work_dir/FreeBSDAArch64LibC.stderr" ] || fail "runtime: FreeBSDAArch64LibC wrote unexpected stderr"

check_trap FreeBSDAArch64Assert "Assertion failed" "native FreeBSD AArch64 assertion fixture"
check_trap FreeBSDAArch64Panic "Panic" "native FreeBSD AArch64 panic fixture"

run_and_capture FreeBSDAArch64Syscall "$syscall_output"
[ "$run_status" -eq 0 ] || fail "runtime: BSD syscall package test exited with $run_status"
require_empty_output FreeBSDAArch64Syscall

run_and_capture FreeBSDAArch64Loader "$output_dir/FreeBSDAArch64Loader"
[ "$run_status" -eq 0 ] || fail "loading/runtime: FreeBSDAArch64Loader exited with $run_status"
require_empty_output FreeBSDAArch64Loader

echo "FreeBSD AArch64 native runtime fixtures passed"
