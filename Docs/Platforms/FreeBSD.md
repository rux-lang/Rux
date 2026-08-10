# Rux on FreeBSD

This guide covers installing and building Rux on x86-64 and AArch64 FreeBSD. Return to the [main README](../../README.md) for language documentation and project information.

## Installing a Release

Download the `rux-freebsd-x86_64.tar.gz` or `rux-freebsd-aarch64.tar.gz` archive
matching the host from the [latest GitHub release](https://github.com/rux-lang/Rux/releases/latest).
Extract it, make `rux` executable, and place it in a directory on `PATH`.

## Building from Source

Rux currently requires Clang 22.1 or newer, CMake 3.30 or newer, Ninja 1.11 or newer, and a recent Git installation. FreeBSD 14 packages a compatible CMake 3.31 release:

```sh
sudo pkg install -y llvm22 cmake ninja git
```

Clone and build Rux:

```sh
git clone https://github.com/rux-lang/Rux.git
cd Rux
sh Build.sh
```

The FreeBSD package names the compiler `clang++22`; `Build.sh` detects it automatically. The script creates a Release build in `Build/` and writes the compiler to `Bin/rux`.

On AArch64, Rux selects the `freebsd-aarch64` target automatically and uses the
platform Clang driver for final native lowering and linking.

## Native Package Artifacts

An `Executable` package writes `Name`, a `SharedLibrary` writes `libName.so` with its SONAME, and a `StaticLibrary` writes `libName.a` with a GNU archive symbol index. Shared and static libraries can be built but not passed to `rux run`. `SourceLibrary` has no standalone native artifact.

The x86-64 backend writes ELF objects and images directly. The AArch64 backend uses Clang for relocatable-object and shared-library emission, then the common deterministic archive layer for static output.

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
