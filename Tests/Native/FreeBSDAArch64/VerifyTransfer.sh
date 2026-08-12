#!/bin/sh

set -eu

payload_dir=${1:-FreeBSDAArch64Payload}

fail() {
    echo "FreeBSD AArch64 transferred runtime failure: $*" >&2
    exit 1
}

[ "$(uname -s)" = "FreeBSD" ] || fail "the payload must run on FreeBSD"
case "$(uname -m)" in
    arm64 | aarch64) ;;
    *) fail "the payload must run in a separate native AArch64 VM" ;;
esac
[ -d "$payload_dir" ] || fail "payload directory '$payload_dir' does not exist"
[ -f "$payload_dir/MANIFEST.tsv" ] || fail "the payload manifest is missing"

work_dir=$(mktemp -d "${TMPDIR:-/tmp}/rux-freebsd-aarch64-transferred.XXXXXX")
trap 'rm -rf "$work_dir"' EXIT HUP INT TERM

byte_count() {
    od -An -tu1 -j "$2" -N "$3" "$1" 2>/dev/null
}

read_le16() {
    set -- $(byte_count "$1" "$2" 2)
    [ "$#" -eq 2 ] || fail "could not read an ELF field from '$1'"
    echo $(( $1 | ($2 << 8) ))
}

preflight_elf() {
    artifact=$1
    expected_type=$2
    [ "$(wc -c <"$artifact" | tr -d ' ')" -ge 64 ] || fail "'$artifact' is smaller than an ELF64 header"
    set -- $(byte_count "$artifact" 0 16)
    [ "$#" -eq 16 ] || fail "could not read the ELF identity from '$artifact'"
    [ "$1 $2 $3 $4 $5 $6 $7 $8" = "127 69 76 70 2 1 1 9" ] || \
        fail "'$artifact' is not a little-endian FreeBSD ELF64 image"
    [ "$(read_le16 "$artifact" 16)" -eq "$expected_type" ] || fail "'$artifact' has the wrong ELF type"
    [ "$(read_le16 "$artifact" 18)" -eq 183 ] || fail "'$artifact' is not AArch64"
}

expected_entry() {
    case "$1" in
        FreeBSDAArch64ExitCode) echo '755 exec exit-73' ;;
        FreeBSDAArch64LibC) echo '755 exec stdout-libc' ;;
        FreeBSDAArch64Assert) echo '755 exec assertion-diagnostic' ;;
        FreeBSDAArch64Syscall) echo '755 exec exit-0-syscalls' ;;
        libFreeBSDAArch64Fixture.so) echo '644 shared load-target' ;;
        FreeBSDAArch64Loader) echo '755 exec exit-0-load-call-unload' ;;
        *) fail "manifest names unexpected artifact '$1'" ;;
    esac
}

tab=$(printf '\t')
exec 3<"$payload_dir/MANIFEST.tsv"
IFS="$tab" read -r signature version <&3
[ "$signature" = "RUX-FREEBSD-AARCH64-PAYLOAD" ] && [ "$version" = 1 ] || fail "unsupported payload manifest"
count=0
seen=' '
while IFS="$tab" read -r name expected_hash declared_mode elf_kind outcome <&3; do
    [ -n "$name" ] || fail "manifest contains an empty artifact name"
    case "$seen" in *" $name "*) fail "manifest repeats '$name'" ;; esac
    seen="$seen$name "
    set -- $(expected_entry "$name")
    [ "$declared_mode $elf_kind $outcome" = "$1 $2 $3" ] || fail "manifest metadata for '$name' is unexpected"
    artifact="$payload_dir/$name"
    [ -f "$artifact" ] || fail "manifest artifact '$name' is missing"
    chmod "$declared_mode" "$artifact"
    [ "$(stat -f '%Lp' "$artifact")" = "$declared_mode" ] || fail "could not restore mode $declared_mode on '$name'"
    [ "$(sha256 -q "$artifact")" = "$expected_hash" ] || fail "SHA-256 mismatch for '$name'"
    if [ "$elf_kind" = exec ]; then
        preflight_elf "$artifact" 2
    else
        preflight_elf "$artifact" 3
    fi
    count=$((count + 1))
done
exec 3<&-
[ "$count" -eq 6 ] || fail "manifest contains $count artifacts instead of 6"

set -- "$payload_dir"/*
[ "$#" -eq 7 ] || fail "payload contains files not declared by its manifest"
for artifact in "$@"; do
    [ -f "$artifact" ] || fail "payload contains a non-file entry"
done

run_and_capture() {
    name=$1
    set +e
    (cd "$payload_dir" && exec "./$name") >"$work_dir/$name.stdout" 2>"$work_dir/$name.stderr"
    run_status=$?
    set -e
}

require_empty_output() {
    name=$1
    [ ! -s "$work_dir/$name.stdout" ] || fail "$name wrote unexpected stdout"
    [ ! -s "$work_dir/$name.stderr" ] || fail "$name wrote unexpected stderr"
}

run_and_capture FreeBSDAArch64ExitCode
[ "$run_status" -eq 73 ] || fail "FreeBSDAArch64ExitCode exited with $run_status instead of 73"
require_empty_output FreeBSDAArch64ExitCode

run_and_capture FreeBSDAArch64LibC
[ "$run_status" -eq 0 ] || fail "FreeBSDAArch64LibC exited with $run_status"
printf 'freebsd-aarch64:42:2.5\n' >"$work_dir/libc.expected"
cmp -s "$work_dir/libc.expected" "$work_dir/FreeBSDAArch64LibC.stdout" || fail "libc fixture output differs"
[ ! -s "$work_dir/FreeBSDAArch64LibC.stderr" ] || fail "libc fixture wrote unexpected stderr"

run_and_capture FreeBSDAArch64Assert
[ "$run_status" -ne 0 ] || fail "assertion fixture unexpectedly succeeded"
[ ! -s "$work_dir/FreeBSDAArch64Assert.stdout" ] || fail "assertion fixture wrote unexpected stdout"
sed -E 's# \([^)]*Src[/\\]Main\.rux:[0-9]+:[0-9]+\)$# (Src/Main.rux:<line>:<column>)#' \
    "$work_dir/FreeBSDAArch64Assert.stderr" >"$work_dir/assert.normalized"
printf '%s\n' 'Assertion failed: native FreeBSD AArch64 assertion fixture' \
    '  at Main (Src/Main.rux:<line>:<column>)' >"$work_dir/assert.expected"
cmp -s "$work_dir/assert.expected" "$work_dir/assert.normalized" || fail "assertion diagnostic differs"

run_and_capture FreeBSDAArch64Syscall
[ "$run_status" -eq 0 ] || fail "BSD syscall fixture exited with $run_status"
require_empty_output FreeBSDAArch64Syscall

run_and_capture FreeBSDAArch64Loader
[ "$run_status" -eq 0 ] || fail "shared-library loader exited with $run_status"
require_empty_output FreeBSDAArch64Loader

echo "Transferred FreeBSD AArch64 runtime payload passed"
