# Rux on macOS

This guide covers installing and building Rux on x86-64 and AArch64 macOS. Return to the [main README](../../README.md) for language documentation and project information.

## Installing a Release

Download the `rux-macos-x86_64.tar.gz` or `rux-macos-aarch64.tar.gz` archive
matching the Mac from the [latest GitHub release](https://github.com/rux-lang/Rux/releases/latest).
Extract it, make `rux` executable, and place it in a directory on `PATH`.

## Building from Source

Rux currently requires Clang 22.1 or newer, CMake 3.30 or newer, Ninja 1.11 or newer, and a recent Git installation. Apple Clang does not yet provide all C++26 features used by Rux, so install upstream LLVM 22 and the build tools with [Homebrew](https://brew.sh/):

```sh
brew install llvm@22 cmake ninja git
```

Clone and build Rux with Homebrew's Clang:

```sh
git clone https://github.com/rux-lang/Rux.git
cd Rux
sh Build.sh --compiler "$(brew --prefix llvm@22)/bin/clang++"
```

The script creates a Release build in `Build/` and writes the compiler to `Bin/rux`.

For a Debug build, add `--configuration Debug`. Run `sh Build.sh --help` to see every option.

On Apple Silicon, Rux selects the `macos-aarch64` target by default, and that
target has no image writer. The AArch64 back end reaches `linux-aarch64`
through ELF and `windows-aarch64` through PE/COFF, but cannot yet write Mach-O.
Building for `macos-aarch64` is refused with `code generation for
'macos-aarch64' is not implemented yet`; `rux check`, `rux fmt`, `rux lint` and
`rux doc` need no back end and work as they do everywhere. An Apple Silicon Mac
can still build `rux` itself, and can cross-build Rux programs with `--target
macos-x86_64`, `--target linux-aarch64`, or `--target windows-aarch64`. An
AArch64 Mach-O writer is what makes the native target buildable, and is not
written yet.

## Native Package Artifacts

An `Executable` package writes `Name`, a `SharedLibrary` writes `libName.dylib` with `LC_ID_DYLIB` set to `@rpath/libName.dylib`, and a `StaticLibrary` writes `libName.a` with a BSD archive symbol index. Shared and static libraries can be built but not passed to `rux run`. `SourceLibrary` has no standalone native artifact.

The x86-64 backend writes Mach-O objects and images directly, from a host of either architecture. There is no AArch64 Mach-O writer, so those artifact kinds have no `macos-aarch64` form yet.

## Verifying the Build

Run the compiler:

```sh
./Bin/rux version
file ./Bin/rux
```

On Apple Silicon, `file` should report a `Mach-O 64-bit executable arm64`.
Programs built by `./Bin/rux build` should report the same architecture.

Run the complete repository verification workflow with the same Homebrew compiler:

```sh
sh Test.sh --compiler "$(brew --prefix llvm@22)/bin/clang++"
```

Static analysis is intentionally opt-in because it is slower:

```sh
sh Test.sh --compiler "$(brew --prefix llvm@22)/bin/clang++" --clang-tidy
```

Use `sh Format.sh` to format maintained C++ and Rux sources, or `sh Format.sh --check` to check them without making changes.

`rux run` always builds and launches the compiler process's host triple and has
no `--target` option. `rux test --target` requires macOS plus an architecture
reported for either the compiler process or the native OS. Consequently, an
x86-64 compiler running under Rosetta on Apple Silicon may directly test
`macos-aarch64` once that image writer is available; a physical Intel Mac may
only build/check that target and transfer it to Apple Silicon for testing.
