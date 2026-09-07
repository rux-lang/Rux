#!/bin/sh

# Pack the macOS toolchain prefix from Homebrew's llvm bottle.
#
# LLVM publishes no macOS archive for this release, so the source is Homebrew,
# pinned by formula version: the pack fails if brew resolves any other LLVM.
# Nothing is compiled here. The bottle is installed on the packing runner, the
# tools Rux uses are copied out together with every library they load, and
# each install name is rewritten so the prefix works wherever it is unpacked.
# ccache comes from its own universal release.
#
# .github/Scripts/SetupToolchain.sh runs this only when the Actions cache held
# no prefix for the current manifest, and the composite action caches the
# result, so this is the cold path: once per pin bump.
#
# Usage: sh .github/Scripts/Toolchain/PackMacOS.sh --target macos-aarch64 --prefix DIR

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
# The x86-64 compiler is cross-built from this prefix on the AArch64 runner and
# tested under Rosetta, so there is no macos-x86_64 prefix to pack.
case "$target" in
macos-aarch64) ;;
*) die "unsupported target '$target'; PackLinux.sh and PackWindows.ps1 cover the others" ;;
esac
[ "$(uname -s)" = Darwin ] || die 'this must run on macOS'

manifest=$repository_root/.github/Toolchains.env
[ -f "$manifest" ] || die "'$manifest' was not found"
if grep -qvE '^[[:space:]]*(#.*)?$|^[A-Z0-9_]+=[A-Za-z0-9._:/+-]*$' "$manifest"; then
    die "'$manifest' contains a line that is not a comment or KEY=VALUE"
fi
. "$manifest"

for pin in "LLVM_VERSION:${LLVM_VERSION:-}" "BREW_LLVM_FORMULA:${BREW_LLVM_FORMULA:-}" \
    "BREW_LLVM_VERSION:${BREW_LLVM_VERSION:-}" "CCACHE_BASE_URL:${CCACHE_BASE_URL:-}" \
    "CCACHE_VERSION:${CCACHE_VERSION:-}" "CCACHE_ASSET_MACOS:${CCACHE_ASSET_MACOS:-}" \
    "SHA256_CCACHE_MACOS:${SHA256_CCACHE_MACOS:-}"; do
    [ -n "${pin#*:}" ] || die "'$manifest' declares no ${pin%%:*}"
done
[ "$SHA256_CCACHE_MACOS" != TBD ] || die 'SHA256_CCACHE_MACOS is still TBD in Toolchains.env'

for tool in brew curl tar shasum otool install_name_tool codesign xcrun; do
    command -v "$tool" >/dev/null 2>&1 || die "required tool '$tool' was not found"
done

llvm_major=${LLVM_VERSION%%.*}
work=${RUNNER_TEMP:-${TMPDIR:-/tmp}}/rux-pack-$target
rm -rf "$work" "$prefix"
mkdir -p "$work/download" "$work/ccache" "$prefix/bin" "$prefix/lib" "$prefix/include"

sha256_of() {
    shasum -a 256 "$1" | cut -d' ' -f1
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

# The runner ships a formula index that lags the current release and does not
# refresh itself, so without this the pack silently resolves an older LLVM.
brew update >/dev/null
if brew list --versions "$BREW_LLVM_FORMULA" >/dev/null 2>&1; then
    brew upgrade "$BREW_LLVM_FORMULA" >/dev/null 2>&1 || true
else
    brew install "$BREW_LLVM_FORMULA"
fi
llvm=$(brew --prefix "$BREW_LLVM_FORMULA")
installed=$(brew list --versions "$BREW_LLVM_FORMULA" | awk '{ print $2 }')
case "$installed" in
"$BREW_LLVM_VERSION"*) ;;
*)
    die "Homebrew has $BREW_LLVM_FORMULA $installed but Toolchains.env pins $BREW_LLVM_VERSION; if the formula has moved on, point BREW_LLVM_FORMULA at the versioned formula rather than accepting a different compiler"
    ;;
esac

# clang++ in a bottle is a full copy of clang, not a symlink, so the prefix
# ships one real binary and links the other spellings to it.
[ -e "$llvm/bin/clang" ] || die "the $BREW_LLVM_FORMULA formula has no bin/clang"
cp "$llvm/bin/clang" "$prefix/bin/clang"
for alias in clang++ clang++-$llvm_major; do
    ln -sf clang "$prefix/bin/$alias"
done
for tool in clang-format clang-tidy llvm-size; do
    [ -e "$llvm/bin/$tool" ] || die "the $BREW_LLVM_FORMULA formula has no bin/$tool"
    cp "$llvm/bin/$tool" "$prefix/bin/$tool-$llvm_major"
    ln -sf "$tool-$llvm_major" "$prefix/bin/$tool"
done
chmod +x "$prefix"/bin/*

# The resource headers are mandatory: clang-tidy cannot parse a translation
# unit without them. Of the runtime libraries only the builtins, which the
# Darwin driver always links, are kept; Rux uses no sanitizer or profiler.
[ -d "$llvm/lib/clang/$llvm_major/include" ] || die "the $BREW_LLVM_FORMULA formula has no resource headers"
mkdir -p "$prefix/lib/clang"
cp -R "$llvm/lib/clang/$llvm_major" "$prefix/lib/clang/$llvm_major"
if [ -d "$prefix/lib/clang/$llvm_major/lib" ]; then
    find "$prefix/lib/clang/$llvm_major/lib" -type f ! -name 'libclang_rt.osx.a' -delete
fi

# Homebrew's clang++ links its own libc++, and its headers are needed to
# compile against it.
[ -d "$llvm/include/c++" ] || die "the $BREW_LLVM_FORMULA formula has no libc++ headers"
cp -R "$llvm/include/c++" "$prefix/include/c++"

# --- Shared-library closure ----------------------------------------------

# Homebrew's LLVM reaches libraries three ways: absolute paths into other
# formulae, such as z3; @rpath references to its own dylibs; and, in a bottle
# that was never installed, @@HOMEBREW_PREFIX@@ placeholders. Only /usr/lib
# and /System are supplied by macOS; everything else has to travel with the
# prefix.
system_library() {
    case "$1" in
    /usr/lib/* | /System/*) return 0 ;;
    *) return 1 ;;
    esac
}

# Directories the dependency walk searches, one per line. It is a file because
# each pipeline stage runs in its own subshell. Homebrew splits its own
# libraries across lib, lib/c++ and lib/unwind, and dependencies of
# dependencies live beside them, so the search path grows as the walk
# discovers directories.
search_path=$work/search-path
printf '%s\n%s\n%s\n' "$llvm/lib" "$llvm/lib/c++" "$llvm/lib/unwind" >"$search_path"

# An @rpath, @loader_path, @executable_path or placeholder reference names a
# file, not a location, so resolve it by name against everything found so far.
resolve_dependency() {
    case "$1" in
    /*)
        [ -f "$1" ] || return 1
        printf '%s' "$1"
        return 0
        ;;
    esac
    name=${1##*/}
    while IFS= read -r directory; do
        if [ -f "$directory/$name" ]; then
            printf '%s' "$directory/$name"
            return 0
        fi
    done <"$search_path"
    return 1
}

# The walk reads the original files, whose install names are still the ones
# Homebrew recorded; the copies are rewritten afterwards.
collect_dependencies() {
    otool -L "$1" | tail -n +2 | awk '{ print $1 }' | while IFS= read -r dependency; do
        system_library "$dependency" && continue
        name=${dependency##*/}
        [ -f "$prefix/lib/$name" ] && continue

        resolved=$(resolve_dependency "$dependency") ||
            die "could not resolve $dependency referenced by $1"

        cp "$resolved" "$prefix/lib/$name"
        chmod u+w "$prefix/lib/$name"
        directory=$(dirname "$resolved")
        grep -qxF "$directory" "$search_path" || printf '%s\n' "$directory" >>"$search_path"
        collect_dependencies "$resolved"
    done
}

for tool in clang clang-format clang-tidy llvm-size; do
    [ -f "$llvm/bin/$tool" ] && collect_dependencies "$llvm/bin/$tool"
done

# A Homebrew binary records absolute install names, so moving it elsewhere
# breaks every reference. Rewrite each one relative to the loading binary,
# give every dylib an @rpath id, and re-sign, since editing a Mach-O
# invalidates its ad-hoc signature. The proof at the end compiles with the
# result from its new location.
for binary in "$prefix"/bin/* "$prefix"/lib/*.dylib; do
    [ -f "$binary" ] && [ ! -L "$binary" ] || continue
    case "$binary" in
    *.dylib)
        loader='@loader_path'
        install_name_tool -id "@rpath/${binary##*/}" "$binary" 2>/dev/null || true
        ;;
    *) loader='@loader_path/../lib' ;;
    esac

    otool -L "$binary" | tail -n +2 | awk '{ print $1 }' | while IFS= read -r dependency; do
        system_library "$dependency" && continue
        name=${dependency##*/}
        # Only repoint what the prefix actually carries; ccache comes from its
        # own portable release and references nothing in lib.
        [ -f "$prefix/lib/$name" ] || continue
        install_name_tool -change "$dependency" "$loader/$name" "$binary" 2>/dev/null || true
    done

    install_name_tool -add_rpath "$loader" "$binary" 2>/dev/null || true
    codesign --force --sign - "$binary" 2>/dev/null || true
done

# --- ccache -------------------------------------------------------------

ccache_archive=$(fetch "$CCACHE_BASE_URL/$CCACHE_ASSET_MACOS" "$SHA256_CCACHE_MACOS")
tar -xf "$ccache_archive" -C "$work/ccache" --strip-components=1
[ -f "$work/ccache/ccache" ] || die "'$CCACHE_ASSET_MACOS' has no ccache binary"
cp "$work/ccache/ccache" "$prefix/bin/ccache"
chmod +x "$prefix/bin/ccache"

# --- Manifest and proof -------------------------------------------------

{
    printf 'target=%s\n' "$target"
    printf 'llvm=%s\n' "$installed"
    printf 'ccache=%s\n' "$CCACHE_VERSION"
} >"$prefix/MANIFEST"

for tool in clang++-$llvm_major clang-format-$llvm_major clang-tidy-$llvm_major ccache; do
    "$prefix/bin/$tool" --version >/dev/null 2>&1 || die "'$prefix/bin/$tool' does not run"
done

# The compiler must work from the prefix alone, with the SDK named the way the
# consumer names it, before any of this is cached.
printf '#include <print>\nint main() { std::println("{}", 42); }\n' >"$work/probe.cpp"
SDKROOT=$(xcrun --show-sdk-path) "$prefix/bin/clang++-$llvm_major" -std=c++26 \
    -o "$work/probe" "$work/probe.cpp" || die 'the packed compiler cannot build a C++26 program'
[ "$("$work/probe")" = 42 ] || die 'the packed compiler produced a program that does not run'

rm -rf "$work"
printf 'Packed %s into %s (%s)\n' "$target" "$prefix" "$(du -sh "$prefix" | cut -f1)"
