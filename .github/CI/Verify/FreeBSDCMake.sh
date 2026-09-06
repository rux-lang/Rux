#!/bin/sh
# Ordinary CI may use an installed package or an already restored private installation.
# Source compilation belongs exclusively to the manual preparation workflow.
set -eu
for candidate in /opt/rux-tools/cmake/bin/cmake "$PWD/BuildCache/CMake/bin/cmake" /usr/local/bin/cmake; do
    if [ -x "$candidate" ] && "$candidate" --version | grep -q '^cmake version 4\.4\.3$'; then
        dirname "$candidate"
        exit 0
    fi
done
echo 'error: CMake 4.4.3 binary is unavailable. Run Prepare FreeBSD Images and promote the validated environment lock; CI will not compile CMake.' >&2
exit 1
