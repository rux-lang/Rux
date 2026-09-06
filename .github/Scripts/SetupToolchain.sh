#!/bin/sh

# Restore the pinned prebuilt toolchain for one target.
#
# CI never installs or compiles a toolchain: it downloads exactly one verified
# bundle from rux-lang/Toolchain and puts it on PATH. The POSIX peer of
# .github/Scripts/SetupToolchain.ps1.
#
# Usage: sh .github/Scripts/SetupToolchain.sh --target linux-x86_64 [--prefix DIR]

set -eu

script_directory=$(CDPATH= cd -P "$(dirname "$0")" && pwd)
repository_root=$(CDPATH= cd -P "$script_directory/../.." && pwd)

target=
prefix=${RUX_TOOLCHAIN:-}

die() {
    printf 'error: %s\n' "$1" >&2
    exit 1
}

while [ "$#" -gt 0 ]; do
    case "$1" in
    --target)
        [ "$#" -ge 2 ] || die "option '--target' requires a value"
        target=$2
        shift 2
        ;;
    --prefix)
        [ "$#" -ge 2 ] || die "option '--prefix' requires a value"
        prefix=$2
        shift 2
        ;;
    *) die "unknown option '$1'" ;;
    esac
done

[ -n "$target" ] || die "option '--target' is required"
# FreeBSD has no hosted runner: its toolchain lives inside the prepared guest
# image, so freebsd-* is handled by freebsd-vm.sh rather than here.
case "$target" in
linux-x86_64 | linux-aarch64 | macos-x86_64 | macos-aarch64) ;;
*) die "unsupported target '$target'" ;;
esac

# Dot-sourcing is safe because the file is a checked-in flat KEY=VALUE list with
# no expansion; reject anything else rather than executing it.
manifest=$repository_root/.github/Toolchains.env
[ -f "$manifest" ] || die "'$manifest' was not found"
if grep -qvE '^[[:space:]]*(#.*)?$|^[A-Z0-9_]+=[A-Za-z0-9._:/+-]*$' "$manifest"; then
    die "'$manifest' contains a line that is not a comment or KEY=VALUE"
fi
. "$manifest"

checksum_variable="SHA256_TOOLCHAIN_$(printf '%s' "$target" | tr 'a-z-' 'A-Z_')"
eval "checksum=\${$checksum_variable:-}"
[ -n "$checksum" ] || die "'$manifest' declares no $checksum_variable"
[ "$checksum" != TBD ] ||
    die "$checksum_variable is still TBD; publish a Toolchain release and record its checksum"
[ "$TOOLCHAIN_REVISION" != TBD ] || die "TOOLCHAIN_REVISION is still TBD"

archive_name=rux-toolchain-$target-$TOOLCHAIN_REVISION.tar.zst
archive_url=$TOOLCHAIN_BASE_URL/toolchain-$TOOLCHAIN_REVISION/$archive_name

if [ -z "$prefix" ]; then
    prefix=${RUNNER_TEMP:-${TMPDIR:-/tmp}}/rux-toolchain
fi

# Stage into .partial and rename only after the checksum matches, so an
# interrupted download can never be mistaken for a verified one.
download() {
    url=$1
    destination=$2
    attempt=1
    while [ "$attempt" -le 3 ]; do
        rm -f "$destination.partial"
        if curl --fail --silent --show-error --location --retry 2 \
            --output "$destination.partial" "$url"; then
            mv "$destination.partial" "$destination"
            return 0
        fi
        printf 'warning: download attempt %s of 3 failed for %s\n' "$attempt" "$url" >&2
        sleep $((attempt * 5))
        attempt=$((attempt + 1))
    done
    die "could not download '$url'"
}

verify() {
    file=$1
    expected=$2
    if command -v sha256sum >/dev/null 2>&1; then
        actual=$(sha256sum "$file" | cut -d' ' -f1)
    elif command -v shasum >/dev/null 2>&1; then
        actual=$(shasum -a 256 "$file" | cut -d' ' -f1)
    else
        actual=$(sha256 -q "$file")
    fi
    [ "$actual" = "$expected" ] ||
        die "checksum mismatch for '$file': expected $expected, got $actual"
}

# The marker records which revision the prefix already holds, so a warm runner
# cache skips download and extraction entirely.
marker=$prefix/.rux-toolchain-revision
if [ -f "$marker" ] && [ "$(cat "$marker")" = "$target-$TOOLCHAIN_REVISION" ]; then
    printf 'Toolchain %s %s is already present in %s\n' "$target" "$TOOLCHAIN_REVISION" "$prefix"
else
    staging=${RUNNER_TEMP:-${TMPDIR:-/tmp}}/rux-toolchain-download
    mkdir -p "$staging"
    printf 'Downloading %s\n' "$archive_url"
    download "$archive_url" "$staging/$archive_name"
    verify "$staging/$archive_name" "$checksum"

    rm -rf "$prefix"
    mkdir -p "$prefix"
    tar --use-compress-program=unzstd -xf "$staging/$archive_name" -C "$prefix" --strip-components=1
    rm -f "$staging/$archive_name"
    printf '%s' "$target-$TOOLCHAIN_REVISION" >"$marker"
fi

[ -x "$prefix/bin/clang++-23" ] || die "'$prefix/bin/clang++-23' is missing from the bundle"

# GITHUB_ENV entries must be single-line; a newline would let a value inject a
# second assignment.
export_variable() {
    case "$2" in
    *"$(printf '\n')"*) die "refusing to export '$1' because its value spans lines" ;;
    esac
    if [ -n "${GITHUB_ENV:-}" ]; then
        printf '%s=%s\n' "$1" "$2" >>"$GITHUB_ENV"
    fi
    printf 'export %s=%s\n' "$1" "$2"
}

export_variable RUX_TOOLCHAIN "$prefix"
export_variable CXX "$prefix/bin/clang++-23"
export_variable CMAKE_CXX_COMPILER_LAUNCHER ccache
export_variable CCACHE_DIR "${GITHUB_WORKSPACE:-$repository_root}/BuildCache/ccache"
export_variable CCACHE_MAXSIZE "$CCACHE_MAXSIZE"
export_variable CCACHE_COMPILERCHECK content
# The FreeBSD guest builds under /root/rux while the host builds under the
# workspace path; without this they would never share cache entries.
export_variable CCACHE_NOHASHDIR 1

if [ -n "${GITHUB_PATH:-}" ]; then
    printf '%s\n' "$prefix/bin" >>"$GITHUB_PATH"
fi

"$prefix/bin/clang++-23" --version | head -1
"$prefix/bin/cmake" --version | head -1
"$prefix/bin/ninja" --version
