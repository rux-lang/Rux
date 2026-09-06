#!/bin/sh
set -eu
scripts=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
task=${1:?usage: Run.sh environment|native-environment|freebsd|prepare-freebsd|tidy|benchmark [arguments]}
shift
case "$task" in
    environment) implementation=Environment.py ;;
    native-environment) implementation=NativeEnvironment.py ;;
    validate-native) implementation=ValidateNative.py ;;
    freebsd) implementation=RunFreeBSD.py ;;
    prepare-freebsd) implementation=PrepareFreeBSD.py ;;
    tidy) implementation=Verify/TidyShard.py ;;
    benchmark) implementation=Benchmark.py ;;
    *) echo "error: unknown CI operation: $task" >&2; exit 2 ;;
esac
exec python3 "$scripts/../CI/$implementation" "$@"
