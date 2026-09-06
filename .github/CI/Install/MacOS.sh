#!/bin/sh
# Cache only the LLVM dependency closure, never the runner's entire Homebrew prefix.
set -eu
installer=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
destination=${1:?toolchain archive directory required}
mkdir -p "$destination"
archive="$destination/homebrew-tools.tar.gz"
prefix=$(brew --prefix)
if [ ! -f "$archive" ] || [ ! -f "$destination/compiler" ]; then
    export HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_INSTALL_CLEANUP=1
    brew update --quiet
    brew install llvm@23 ccache
    compiler="$(brew --prefix llvm@23)/bin/clang++"
    sh "$installer/../Verify/LLVM.sh" "$compiler"
    # Resolve the directory, preserving clang++: resolving the executable's
    # symlink to clang-23 selects the C driver and drops C++ runtime linkage.
    compiler="$(CDPATH= cd -- "$(dirname -- "$compiler")" && pwd -P)/clang++"
    # Multiple formulae default to their intersection, omitting LLVM-only Z3.
    brew deps --union --installed --formula llvm@23 ccache > "$destination/formulae"
    printf '%s\n' llvm@23 ccache >> "$destination/formulae"
    : > "$destination/paths"
    while IFS= read -r formula; do
        brew --prefix "$formula"
    done < "$destination/formulae" > "$destination/prefixes"
    python3 - "$prefix" "$destination" <<'PY'
import os
from pathlib import Path
import sys
root, output = Path(sys.argv[1]), Path(sys.argv[2])
versions = {Path(line).resolve() for line in (output / 'prefixes').read_text().splitlines()}
for version in versions:
    if not version.is_relative_to(root / 'Cellar'):
        raise SystemExit('Dependency outside the Homebrew Cellar')
paths = {str(p.relative_to(root)) for p in versions}
for link in (root / 'opt').iterdir():
    if link.is_symlink() and link.resolve() in versions:
        paths.add(str(link.relative_to(root)))
(output / 'paths').write_text('\n'.join(sorted(paths)) + '\n')
PY
    tar -czf "$archive.partial" -C "$prefix" -T "$destination/paths"
    printf '%s\n' "$compiler" > "$destination/compiler"
    printf '%s\n' "$prefix" > "$destination/prefix"
    mv "$archive.partial" "$archive"
fi
[ "$(cat "$destination/prefix")" = "$prefix" ] || { echo 'error: Homebrew prefix mismatch' >&2; exit 1; }
tar -xzf "$archive" -C "$prefix"
compiler=$(cat "$destination/compiler")
# Repair metadata from older bundles that recorded the C-driver symlink target.
compiler="$(CDPATH= cd -- "$(dirname -- "$compiler")" && pwd -P)/clang++"
printf '%s\n' "$compiler" > "$destination/compiler"
sh "$installer/../Verify/LLVM.sh" "$compiler"
"$prefix/opt/ccache/bin/ccache" --version
if [ -n "${GITHUB_PATH:-}" ]; then
    printf '%s\n' "$(dirname "$compiler")" "$prefix/opt/ccache/bin" >> "$GITHUB_PATH"
fi
if [ -n "${GITHUB_OUTPUT:-}" ]; then printf 'compiler=%s\n' "$compiler" >> "$GITHUB_OUTPUT"; fi
