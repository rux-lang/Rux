#!/bin/sh
# Package repositories advance patch releases; retain the LLVM 23 analysis baseline.
set -eu
output=$("$@" --version)
printf '%s\n' "$output"
version=$(printf '%s\n' "$output" | awk '/clang version/ { for (i = 1; i < NF; i++) if ($i == "version") { print $(i + 1); exit } }')
case "$output" in *Apple*) version=unsupported ;; esac
case "$version" in
    23.*)
        remainder=${version#23.}
        minor=${remainder%%.*}
        case "$minor" in
            ''|*[!0-9]*) ;;
            *) if [ "$minor" -ge 1 ]; then exit 0; fi ;;
        esac
        ;;
esac
echo 'error: expected upstream LLVM 23.1 or newer within major 23' >&2
exit 1
