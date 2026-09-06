#!/bin/sh
set -eu
scripts=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tool=${1:?usage: Install.sh linux-llvm|macos-llvm|build-tools|freebsd-cmake-source [arguments]}
shift
case "$tool" in
    linux-llvm) exec sh "$scripts/../CI/Install/Linux.sh" "$@" ;;
    macos-llvm) exec sh "$scripts/../CI/Install/MacOS.sh" "$@" ;;
    build-tools) exec pwsh -NoProfile -File "$scripts/Install.ps1" -Tool BuildTools "$@" ;;
    # Explicit bootstrap operation, never invoked by push/PR/release consumers.
    freebsd-cmake-source) exec sh "$scripts/../CI/Install/FreeBSDCMake.sh" "$@" ;;
    *) echo "error: unknown installation operation: $tool" >&2; exit 2 ;;
esac
