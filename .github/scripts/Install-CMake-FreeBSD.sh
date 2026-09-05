#!/bin/sh
# FreeBSD packages can lag the required CMake release. Build a private, cacheable installation from verified sources.
set -eu

version=4.4.3
checksum=c46400618b4f1f2b43507f24fb22f3ae830c3416cf23b776e16e1d413aa892f0
destination=${1:-BuildCache/CMake}
mkdir -p "$destination"
destination=$(cd "$destination" && pwd)
if [ -x "$destination/bin/cmake" ] && "$destination/bin/cmake" --version | grep -q "^cmake version $version$"; then
    exit 0
fi

archive="$destination/cmake-$version.tar.gz"
fetch -o "$archive" "https://github.com/Kitware/CMake/releases/download/v$version/cmake-$version.tar.gz"
[ "$(sha256 -q "$archive")" = "$checksum" ] || { echo 'error: CMake archive checksum mismatch' >&2; exit 1; }
tar -xf "$archive" -C "$destination"
cmake -S "$destination/cmake-$version" -B "$destination/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$destination" \
    -DBUILD_TESTING=OFF -DCMAKE_USE_OPENSSL=OFF
cmake --build "$destination/build" --parallel 4
cmake --install "$destination/build"
"$destination/bin/cmake" --version
