#!/bin/sh
set -eu
scripts=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
check=${1:?usage: Verify.sh freebsd-cmake|infrastructure [arguments]}
shift
case "$check" in
    freebsd-cmake) exec sh "$scripts/../CI/Verify/FreeBSDCMake.sh" "$@" ;;
    infrastructure) exec python3 "$scripts/../../Tests/Scripts/CI/Check.py" "$@" ;;
    *) echo "error: unknown verification operation: $check" >&2; exit 2 ;;
esac
