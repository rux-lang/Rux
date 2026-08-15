#!/bin/sh

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../../.." && pwd)
rux=${1:-"$repo_root/Bin/rux"}
payload_dir=${2:-"$repo_root/FreeBSDAArch64Payload"}
output_dir="$repo_root/Bin/Tests/Native/Release/FreeBSD/AArch64"
syscall_output="$repo_root/Bin/Tests/Packages/FreeBSD/Release/FreeBSD/AArch64/Syscall"

fail() {
    echo "FreeBSD AArch64 transfer build failure: $*" >&2
    exit 1
}

[ "$(uname -s)" = "FreeBSD" ] || fail "the payload must be produced on FreeBSD"
case "$(uname -m)" in
    amd64 | x86_64) ;;
    *) fail "the payload must be produced by the compiler in an x86-64 FreeBSD VM" ;;
esac
[ -f "$rux" ] || fail "compiler '$rux' does not exist"
[ -n "$payload_dir" ] && [ "$payload_dir" != "/" ] || fail "invalid payload directory"
[ ! -e "$payload_dir" ] || fail "payload directory '$payload_dir' already exists"

work_dir=$(mktemp -d "${TMPDIR:-/tmp}/rux-freebsd-aarch64-transfer.XXXXXX")
trap 'rm -rf "$work_dir"' EXIT HUP INT TERM

build_fixture() {
    fixture=$1
    name=$2
    log="$work_dir/$name.build.log"
    if ! "$rux" --manifest "$script_dir/$fixture/Fixture.toml" build --release \
        --target freebsd-aarch64 --quiet >"$log" 2>&1; then
        cat "$log" >&2
        fail "cross-compilation failed while building $name"
    fi
    [ ! -s "$log" ] || fail "$name produced unexpected build output"
}

build_fixture ExitCode FreeBSDAArch64ExitCode
build_fixture LibC FreeBSDAArch64LibC
build_fixture Assert FreeBSDAArch64Assert
build_fixture Shared/Library FreeBSDAArch64Fixture
build_fixture Shared/Loader FreeBSDAArch64Loader

syscall_log="$work_dir/FreeBSDAArch64Syscall.build.log"
if ! "$rux" --manifest "$repo_root/Tests/Packages/FreeBSD/Syscall/Rux.toml" build --release \
    --target freebsd-aarch64 --quiet >"$syscall_log" 2>&1; then
    cat "$syscall_log" >&2
    fail "cross-compilation failed while building the BSD syscall fixture"
fi
[ ! -s "$syscall_log" ] || fail "the BSD syscall fixture produced unexpected build output"

case "$output_dir" in
    */freebsd-aarch64) ;;
    *) fail "fixture output path does not contain the canonical target" ;;
esac
case "$syscall_output" in
    */freebsd-aarch64/*) ;;
    *) fail "syscall output path does not contain the canonical target" ;;
esac

staging="$work_dir/payload"
mkdir "$staging"
manifest="$staging/MANIFEST.tsv"
printf 'RUX-FREEBSD-AARCH64-PAYLOAD\t1\n' >"$manifest"

add_artifact() {
    source=$1
    name=$2
    mode=$3
    elf_type=$4
    outcome=$5
    [ -f "$source" ] || fail "cross-compilation did not produce '$source'"
    cp "$source" "$staging/$name"
    chmod "$mode" "$staging/$name"
    hash=$(sha256 -q "$staging/$name")
    printf '%s\t%s\t%s\t%s\t%s\n' "$name" "$hash" "$mode" "$elf_type" "$outcome" >>"$manifest"
}

add_artifact "$output_dir/FreeBSDAArch64ExitCode" FreeBSDAArch64ExitCode 755 exec exit-73
add_artifact "$output_dir/FreeBSDAArch64LibC" FreeBSDAArch64LibC 755 exec stdout-libc
add_artifact "$output_dir/FreeBSDAArch64Assert" FreeBSDAArch64Assert 755 exec assertion-diagnostic
add_artifact "$syscall_output" FreeBSDAArch64Syscall 755 exec exit-0-syscalls
add_artifact "$output_dir/libFreeBSDAArch64Fixture.so" libFreeBSDAArch64Fixture.so 644 shared load-target
add_artifact "$output_dir/FreeBSDAArch64Loader" FreeBSDAArch64Loader 755 exec exit-0-load-call-unload

mv "$staging" "$payload_dir"
echo "FreeBSD x86-64 cross-compiler payload created at $payload_dir"
