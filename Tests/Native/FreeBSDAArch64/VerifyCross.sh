#!/bin/sh

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../../.." && pwd)
rux=${1:-"$repo_root/Bin/rux"}
executable="$repo_root/Bin/Tests/Language/Release/freebsd-aarch64/Arithmetic"
archive="$repo_root/Bin/Tests/Native/Release/freebsd-aarch64/libFreeBSDAArch64Static.a"

fail() {
    echo "FreeBSD AArch64 cross-build smoke failure: $*" >&2
    exit 1
}

[ -f "$rux" ] || fail "compiler '$rux' does not exist"
"$rux" check --target freebsd-aarch64
"$rux" --manifest "$repo_root/Tests/Language/Arithmetic/Rux.toml" build --release --target freebsd-aarch64
"$rux" --manifest "$script_dir/Static/Fixture.toml" build --release --target freebsd-aarch64

byte_count() {
    od -An -tu1 -j "$2" -N "$3" "$1" 2>/dev/null
}

verify_elf_header() {
    artifact=$1
    expected_type=$2
    base_offset=$3
    set -- $(byte_count "$artifact" "$base_offset" 20)
    [ "$#" -eq 20 ] || fail "could not read the ELF header from '$artifact'"
    [ "$1 $2 $3 $4 $5 $6 $7 $8" = "127 69 76 70 2 1 1 9" ] || fail "'$artifact' is not FreeBSD ELF64"
    [ $(( ${17} | (${18} << 8) )) -eq "$expected_type" ] || fail "'$artifact' has the wrong ELF type"
    [ $(( ${19} | (${20} << 8) )) -eq 183 ] || fail "'$artifact' is not AArch64"
}

case "$executable" in */freebsd-aarch64/*) ;; *) fail "executable path is not target-separated" ;; esac
case "$archive" in */freebsd-aarch64/*) ;; *) fail "archive path is not target-separated" ;; esac
verify_elf_header "$executable" 2 0

[ "$(dd if="$archive" bs=1 count=8 2>/dev/null)" = '!<arch>' ] || fail "static library has no archive magic"
archive_size=$(wc -c <"$archive" | tr -d ' ')
offset=8
object_count=0
while [ "$offset" -lt "$archive_size" ]; do
    [ $((offset + 60)) -le "$archive_size" ] || fail "archive member header extends beyond the file"
    set -- $(byte_count "$archive" $((offset + 58)) 2)
    [ "$1 $2" = "96 10" ] || fail "archive member header has an invalid trailer"
    member_size=$(dd if="$archive" bs=1 skip=$((offset + 48)) count=10 2>/dev/null | tr -d ' ')
    case "$member_size" in '' | *[!0-9]*) fail "archive member has an invalid size" ;; esac
    data_offset=$((offset + 60))
    [ $((data_offset + member_size)) -le "$archive_size" ] || fail "archive member extends beyond the file"
    set -- $(byte_count "$archive" "$data_offset" 4)
    if [ "$#" -eq 4 ] && [ "$1 $2 $3 $4" = "127 69 76 70" ]; then
        verify_elf_header "$archive" 1 "$data_offset"
        object_count=$((object_count + 1))
    fi
    offset=$((data_offset + member_size + member_size % 2))
done
[ "$offset" -eq "$archive_size" ] || fail "archive member alignment is invalid"
[ "$object_count" -gt 0 ] || fail "static library contains no AArch64 ELF object"

echo "FreeBSD AArch64 portable cross-build smoke passed"
