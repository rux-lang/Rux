#!/bin/sh

# Prepare the pinned toolchain prefix for one POSIX host and export it.
#
# CI installs no toolchain. The composite action restores the prefix from the
# Actions cache; this script packs it from the pinned upstream assets only when
# the cache held nothing for the current manifest, checks the CMake and Ninja
# the runner image ships, and exports the compiler and ccache settings every
# later step uses. The Windows peer is SetupToolchain.ps1. FreeBSD guests
# install packages instead and export their environment through FreeBSDEnv.sh.
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
# macOS x86-64 is cross-built from the AArch64 prefix, and FreeBSD has no
# hosted runner: its guests install packages.
case "$target" in
linux-x86_64 | linux-aarch64) packer=PackLinux.sh ;;
macos-aarch64) packer=PackMacOS.sh ;;
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
[ -n "${CCACHE_MAXSIZE:-}" ] || die "'$manifest' declares no CCACHE_MAXSIZE"

packer_path=$script_directory/Toolchain/$packer
[ -f "$packer_path" ] || die "'$packer_path' was not found"

if [ -z "$prefix" ]; then
    prefix=${RUNNER_TEMP:-${TMPDIR:-/tmp}}/rux-toolchain
fi

sha256_of_stdin() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum | cut -d' ' -f1
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 | cut -d' ' -f1
    else
        sha256 -q
    fi
}

# The marker records which manifest and packer produced the prefix, so a
# restored cache is used only when both still match; the cache key hashes the
# same files, so a mismatch here means a stale prefix, not a stale key.
revision=$target-$(cat "$manifest" "$packer_path" | sha256_of_stdin)
marker=$prefix/.rux-toolchain-revision
if [ -f "$marker" ] && [ "$(cat "$marker")" = "$revision" ]; then
    printf 'Toolchain %s is already present in %s\n' "$target" "$prefix"
else
    printf 'Packing the %s toolchain into %s\n' "$target" "$prefix"
    rm -rf "$prefix"
    sh "$packer_path" --target "$target" --prefix "$prefix"
    printf '%s' "$revision" >"$marker"
fi

for tool in clang++-23 clang-format-23 clang-tidy-23 ccache; do
    [ -x "$prefix/bin/$tool" ] || die "'$prefix/bin/$tool' is missing from the toolchain prefix"
done

# CMake and Ninja come from the runner image, which ships versions inside the
# range CMakeLists.txt accepts; checking here names the real cause when an
# image changes rather than leaving it to the configure step.
version_at_least() {
    printf '%s\n%s\n' "$1" "$2" | awk -F. '
        NR == 1 { for (i = 1; i <= NF; i++) have[i] = $i + 0; n = NF }
        NR == 2 { for (i = 1; i <= NF; i++) want[i] = $i + 0; if (NF > n) n = NF }
        END {
            for (i = 1; i <= n; i++) {
                if (have[i] + 0 > want[i] + 0) exit 0
                if (have[i] + 0 < want[i] + 0) exit 1
            }
            exit 0
        }'
}

command -v cmake >/dev/null 2>&1 ||
    die 'cmake was not found on PATH; the runner image is expected to ship CMake 3.31 or newer'
cmake_version=$(cmake --version | head -1 | sed 's/^[^0-9]*//; s/[^0-9.].*$//')
version_at_least "$cmake_version" 3.31 ||
    die "the runner image ships CMake $cmake_version but Rux requires 3.31 or newer"
command -v ninja >/dev/null 2>&1 ||
    die 'ninja was not found on PATH; the runner image is expected to ship Ninja 1.13.2 or newer'
ninja_version=$(ninja --version | head -1 | sed 's/[^0-9.].*$//')
version_at_least "$ninja_version" 1.13.2 ||
    die "the runner image ships Ninja $ninja_version but Rux requires 1.13.2 or newer"

# GITHUB_ENV entries must be single-line; a newline would let a value inject a
# second assignment.
#
# The pattern holds a literal newline because command substitution strips
# trailing newlines: $(printf '\n') is the empty string, and matching against
# that rejects every value rather than only the dangerous ones.
newline='
'

export_variable() {
    case "$2" in
    *"$newline"*) die "refusing to export '$1' because its value spans lines" ;;
    esac
    if [ -n "${GITHUB_ENV:-}" ]; then
        printf '%s=%s\n' "$1" "$2" >>"$GITHUB_ENV"
    fi
    printf 'export %s=%s\n' "$1" "$2"
}

export_variable RUX_TOOLCHAIN "$prefix"
export_variable CXX "$prefix/bin/clang++-23"

# The macOS prefix is repacked from Homebrew, whose clang carries no built-in
# SDK path and expects either a Command Line Tools install at a fixed location
# or a configuration file naming the SDK. Neither is guaranteed on a runner, so
# name it the way the Darwin driver expects; without this every standard header
# that forwards to a C one fails to resolve.
if [ "$(uname -s)" = Darwin ]; then
    export_variable SDKROOT "$(xcrun --show-sdk-path)"
fi
export_variable CMAKE_CXX_COMPILER_LAUNCHER ccache
export_variable CCACHE_DIR "${GITHUB_WORKSPACE:-$repository_root}/BuildCache/ccache"
export_variable CCACHE_MAXSIZE "$CCACHE_MAXSIZE"
export_variable CCACHE_COMPILERCHECK content
# Other runs build the same tree at another path; without this they would
# never share cache entries.
export_variable CCACHE_NOHASHDIR 1

if [ -n "${GITHUB_PATH:-}" ]; then
    printf '%s\n' "$prefix/bin" >>"$GITHUB_PATH"
fi

"$prefix/bin/clang++-23" --version | head -1
"$prefix/bin/ccache" --version | head -1
cmake --version | head -1
ninja --version
