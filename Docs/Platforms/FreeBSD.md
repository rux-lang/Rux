# Rux on FreeBSD

This guide covers installing and building Rux on x86-64 FreeBSD. Return to the [main README](../../README.md) for language documentation and project information.

## Installing a Release

A native FreeBSD release is not currently published. Build Rux from source using the instructions below.

## Building from Source

Rux currently requires Clang 22.1 or newer, CMake 3.31 or newer, Ninja 1.11 or newer, and a recent Git installation.

Install the toolchain on FreeBSD 15.1:

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
