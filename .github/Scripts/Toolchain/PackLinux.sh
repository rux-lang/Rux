#!/bin/sh

# Pack the Linux toolchain prefix for one target from upstream releases.
#
# Nothing is compiled here. LLVM publishes prebuilt Linux archives and ccache
# a static binary; this takes the handful of tools Rux uses, gives them the
# names Rux's tool discovery probes for, and writes one relocatable prefix.
# .github/Scripts/SetupToolchain.sh runs it only when the Actions cache held
# no prefix for the current manifest, and the composite action caches the
# result, so this is the cold path: once per pin bump per platform.
#
# Usage: sh .github/Scripts/Toolchain/PackLinux.sh --target linux-x86_64|linux-aarch64 --prefix DIR

set -eu

script_directory=$(CDPATH= cd -P "$(dirname "$0")" && pwd)
repository_root=$(CDPATH= cd -P "$script_directory/../../.." && pwd)

target=
prefix=

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
[ -n "$prefix" ] || die "option '--prefix' is required"
case "$target" in
linux-x86_64 | linux-aarch64) ;;
*) die "unsupported target '$target'; PackMacOS.sh and PackWindows.ps1 cover the others" ;;
esac

# Dot-sourcing is safe because the file is a checked-in flat KEY=VALUE list with
# no expansion; reject anything else rather than executing it.
manifest=$repository_root/.github/Toolchains.env
[ -f "$manifest" ] || die "'$manifest' was not found"
if grep -qvE '^[[:space:]]*(#.*)?$|^[A-Z0-9_]+=[A-Za-z0-9._:/+-]*$' "$manifest"; then
    die "'$manifest' contains a line that is not a comment or KEY=VALUE"
fi
. "$manifest"

slot=$(printf '%s' "$target" | tr 'a-z-' 'A-Z_')
eval "llvm_asset=\${LLVM_ASSET_$slot:-}"
eval "llvm_sha=\${SHA256_LLVM_$slot:-}"
eval "ccache_asset=\${CCACHE_ASSET_$slot:-}"
eval "ccache_sha=\${SHA256_CCACHE_$slot:-}"
for pin in "LLVM_BASE_URL:${LLVM_BASE_URL:-}" "LLVM_VERSION:${LLVM_VERSION:-}" \
    "LLVM_ASSET_$slot:$llvm_asset" "SHA256_LLVM_$slot:$llvm_sha" \
    "CCACHE_BASE_URL:${CCACHE_BASE_URL:-}" "CCACHE_VERSION:${CCACHE_VERSION:-}" \
    "CCACHE_ASSET_$slot:$ccache_asset" "SHA256_CCACHE_$slot:$ccache_sha"; do
    [ -n "${pin#*:}" ] || die "'$manifest' declares no ${pin%%:*}"
done

# A pin that is never checked is decoration, and a checksum still recorded as
# TBD names an input nobody verified, so both stop the pack before any download.
for pin in "SHA256_LLVM_$slot:$llvm_sha" "SHA256_CCACHE_$slot:$ccache_sha"; do
    [ "${pin#*:}" != TBD ] || die "${pin%%:*} is still TBD in Toolchains.env"
done

for tool in curl tar unzstd sha256sum; do
    command -v "$tool" >/dev/null 2>&1 || die "required tool '$tool' was not found"
done

llvm_major=${LLVM_VERSION%%.*}
work=${RUNNER_TEMP:-${TMPDIR:-/tmp}}/rux-pack-$target
rm -rf "$work" "$prefix"
mkdir -p "$work/download" "$work/llvm" "$work/ccache" "$prefix/bin" "$prefix/lib"

sha256_of() {
    sha256sum "$1" | cut -d' ' -f1
}

# Stage into .partial and rename only after the checksum matches, so an
# interrupted download can never be mistaken for a verified one.
fetch() {
    url=$1
    expected=$2
    name=${url##*/}
    curl --fail --silent --show-error --location --retry 3 \
        --output "$work/download/$name.partial" "$url" || die "could not download '$url'"
    actual=$(sha256_of "$work/download/$name.partial")
    [ "$actual" = "$expected" ] ||
        die "checksum mismatch for '$name': expected $expected, got $actual"
    mv "$work/download/$name.partial" "$work/download/$name"
    printf '%s' "$work/download/$name"
}

# --- LLVM ---------------------------------------------------------------

llvm_archive=$(fetch "$LLVM_BASE_URL/$llvm_asset" "$llvm_sha")

# The archive holds gigabytes of tools and static libraries nothing here uses,
# so its members are listed once and only the ones Rux needs are extracted:
# the compiler under every spelling the archive gives it, the two analysis
# tools, llvm-size, and the resource directory, without which clang-tidy
# cannot parse a translation unit.
tar --use-compress-program=unzstd -tf "$llvm_archive" >"$work/members"
llvm_top=$(head -1 "$work/members" | cut -d/ -f1)
[ -n "$llvm_top" ] || die "could not read the top-level directory of '$llvm_asset'"
grep -E "^$llvm_top/(bin/(clang|clang-[0-9]+|clang\\+\\+|clang-format|clang-tidy|llvm-size)$|lib/clang/$llvm_major/(include|lib)/)" \
    "$work/members" >"$work/selected" ||
    die "'$llvm_asset' has none of the expected members"
# Every selected child is listed explicitly. Recursive directory matching
# consumes those children early and GNU tar then reports them as missing.
tar --use-compress-program=unzstd -xf "$llvm_archive" -C "$work/llvm" \
    --no-recursion --strip-components=1 -T "$work/selected"
rm -f "$llvm_archive"
llvm=$work/llvm

# clang++ in an upstream archive is a full copy of clang, not a symlink, so the
# prefix ships one real binary and links the other spellings to it; the driver
# mode comes from argv[0]. cp -L resolves clang when the archive spells it as a
# link to a versioned binary.
[ -e "$llvm/bin/clang" ] || die "'$llvm_asset' has no bin/clang"
cp -L "$llvm/bin/clang" "$prefix/bin/clang"
for alias in clang++ clang++-$llvm_major; do
    ln -sf clang "$prefix/bin/$alias"
done
for tool in clang-format clang-tidy llvm-size; do
    [ -e "$llvm/bin/$tool" ] || die "'$llvm_asset' has no bin/$tool"
    cp -L "$llvm/bin/$tool" "$prefix/bin/$tool-$llvm_major"
    ln -sf "$tool-$llvm_major" "$prefix/bin/$tool"
done
chmod +x "$prefix"/bin/*

# clang finds its resource directory at ../lib/clang/<major> relative to the
# real binary. The headers are mandatory; of the runtime libraries only the
# builtins could ever be linked, since Rux uses no sanitizer or profiler.
[ -d "$llvm/lib/clang/$llvm_major/include" ] || die "'$llvm_asset' has no resource headers"
mkdir -p "$prefix/lib/clang"
cp -R "$llvm/lib/clang/$llvm_major" "$prefix/lib/clang/$llvm_major"
if [ -d "$prefix/lib/clang/$llvm_major/lib" ]; then
    find "$prefix/lib/clang/$llvm_major/lib" -type f ! -name 'libclang_rt.builtins*' -delete
fi

# Upstream links its Linux tools statically: they need only the host's libc,
# libstdc++, libgcc_s and libz, which every runner supplies. libclang-cpp.so
# is built for other consumers and nothing here loads it.

# --- ccache -------------------------------------------------------------

ccache_archive=$(fetch "$CCACHE_BASE_URL/$ccache_asset" "$ccache_sha")
tar -xf "$ccache_archive" -C "$work/ccache" --strip-components=1
[ -f "$work/ccache/ccache" ] || die "'$ccache_asset' has no ccache binary"
cp "$work/ccache/ccache" "$prefix/bin/ccache"
chmod +x "$prefix/bin/ccache"

# --- Manifest and proof -------------------------------------------------

{
    printf 'target=%s\n' "$target"
    printf 'llvm=%s\n' "$LLVM_VERSION"
    printf 'ccache=%s\n' "$CCACHE_VERSION"
} >"$prefix/MANIFEST"

# A tool that cannot start, or that reaches a library the runner does not
# have, is a packing error to find here rather than in the first build.
for tool in clang++-$llvm_major clang-format-$llvm_major clang-tidy-$llvm_major ccache; do
    "$prefix/bin/$tool" --version >/dev/null 2>&1 || die "'$prefix/bin/$tool' does not run"
    if ldd "$prefix/bin/$tool" 2>/dev/null | grep -q 'not found'; then
        die "'$prefix/bin/$tool' needs a library the host does not provide"
    fi
done

rm -rf "$work"
printf 'Packed %s into %s (%s)\n' "$target" "$prefix" "$(du -sh "$prefix" | cut -f1)"
