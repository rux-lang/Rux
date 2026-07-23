# Rux on FreeBSD

This guide covers installing and building Rux on x86-64 and AArch64 FreeBSD. Return to the [main README](../../README.md) for language documentation and project information.

## Installing a Release

A native FreeBSD release is not currently published. Build Rux from source using the instructions below.

## Building from Source

Rux currently requires Clang 22.1 or newer, CMake 4.3 or newer, Ninja 1.11 or newer, and a recent Git installation.

FreeBSD currently packages CMake 3.31, so install it as a bootstrap compiler and
build the pinned CMake 4.3.3 release from source:

```sh
sudo pkg install -y llvm22 cmake ninja git
cmake_version=4.3.3
cmake_archive="/tmp/cmake-${cmake_version}.tar.gz"
cmake_prefix="$HOME/.local/cmake-${cmake_version}"
fetch -o "$cmake_archive" \
  "https://github.com/Kitware/CMake/releases/download/v${cmake_version}/cmake-${cmake_version}.tar.gz"
test "$(sha256 -q "$cmake_archive")" = \
  "cba4bb7a44edf2877bb6f059932896383babe435b3a8c3b5df48b4aa41c9bb85"
tar -xzf "$cmake_archive" -C /tmp
cmake -S "/tmp/cmake-${cmake_version}" -B /tmp/cmake-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang22 \
  -DCMAKE_CXX_COMPILER=clang++22 \
  -DCMAKE_INSTALL_PREFIX="$cmake_prefix" \
  -DBUILD_TESTING=OFF
cmake --build /tmp/cmake-build --parallel
cmake --install /tmp/cmake-build
export PATH="$cmake_prefix/bin:$PATH"
```

Add the final `PATH` export to your shell profile so later sessions use CMake
4.3.3 instead of the bootstrap package.

Clone and build Rux:

```sh
git clone https://github.com/rux-lang/Rux.git
cd Rux
sh Build.sh
```

The FreeBSD package names the compiler `clang++22`; `Build.sh` detects it automatically. The script creates a Release build in `Build/` and writes the compiler to `Bin/rux`.

On AArch64, Rux selects the `freebsd-aarch64` target automatically and uses the
platform Clang driver for final native lowering and linking.

For a Debug build, run `sh Build.sh --configuration Debug`. Run `sh Build.sh --help` to see every option.

## Verifying the Build

Run the compiler:

```sh
./Bin/rux version
```

Run the complete repository verification workflow:

```sh
sh Test.sh
```

Static analysis is intentionally opt-in because it is slower and requires `clang-tidy` from the LLVM 22 package:

```sh
sh Test.sh --clang-tidy
```

Use `sh Format.sh` to format maintained C++ and Rux sources, or `sh Format.sh --check` to check them without making changes.
