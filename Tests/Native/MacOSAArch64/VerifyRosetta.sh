#!/bin/sh

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../../.." && pwd)
rux=${1:-"$repo_root/Bin/rux"}

if [ "$(uname -s)" != "Darwin" ] || [ "$(uname -m)" != "arm64" ]; then
    echo "Rosetta fixtures require an underlying Apple Silicon macOS host" >&2
    exit 1
fi
if [ ! -f "$rux" ]; then
    echo "Compiler '$rux' does not exist" >&2
    exit 1
fi

set -- $(od -An -tu1 -j 4 -N 4 "$rux" 2>/dev/null)
if [ "$#" -ne 4 ] || [ "$1 $2 $3 $4" != "7 0 0 1" ]; then
    echo "Compiler '$rux' is not a thin x86-64 Mach-O image" >&2
    exit 1
fi

if ! /usr/bin/arch -x86_64 /usr/bin/true 2>/dev/null; then
    echo "Rosetta is unavailable on this Apple Silicon host" >&2
    exit 1
fi

RUX_COMPILER_ARCH=x86_64 exec sh "$script_dir/Verify.sh" "$rux"
