#!/bin/sh
# Default repository/CI test parallelism, bounded by available processors.
set -eu
jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1')
[ "$jobs" -ge 1 ] || jobs=1
[ "$jobs" -le 4 ] || jobs=4
printf '%s\n' "$jobs"
