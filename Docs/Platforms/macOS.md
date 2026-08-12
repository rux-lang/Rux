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

On Apple Silicon, Rux selects the `macos-aarch64` target by default. The
AArch64 back end reaches `linux-aarch64` through ELF and `windows-aarch64`
through PE/COFF, and `macos-aarch64` through Mach-O. Its Mach-O paths write
ARM64 object members, deterministic BSD static archives, signed executable
images with or without C function imports, and signed shared libraries. All
three artifact kinds are available through `rux build --target macos-aarch64`
on every supported compiler host. `rux check --target macos-aarch64` selects
the Apple target conditions and ARM64 ABI without producing or executing an
artifact.

The `arm64` alias canonicalizes to `aarch64`, so these commands select the same
target and output directory:

```sh
./Bin/rux check --target macos-arm64
./Bin/rux build --release --target macos-aarch64
```

## Native Package Artifacts

An `Executable` package writes `Name`, a `SharedLibrary` writes `libName.dylib` with `LC_ID_DYLIB` set to `@rpath/libName.dylib`, and a `StaticLibrary` writes `libName.a` with a BSD archive symbol index. Shared and static libraries can be built but not passed to `rux run`. `SourceLibrary` has no standalone native artifact.

The x86-64 backend writes Mach-O objects and images directly, from a host of either architecture. The AArch64 backend writes relocatable Mach-O objects and the BSD archive used for static libraries, including ARM64 branch, page, page-offset, pointer, and explicit-addend relocations. It links freestanding executables with 16 KiB segments, the macOS 26 build-version command, an ARM64 thread entry and exit-syscall stub, and an in-process ad-hoc signature. Executables with C imports use dyld, eagerly bound non-lazy pointers, and Apple ARM64 X16 symbol stubs. Shared libraries use the same stubs for external calls, export public code and data through dyld metadata, rebase image-local pointers, omit executable entry commands, and carry deterministic `@rpath/libName.dylib` identities. Imported data remains unavailable until GOT-aware lowering is implemented.

For a foreign compiler host, a Release cross-build adds the canonical target
below the configured output directory. With the default `Output = "Bin"`, the
three package kinds therefore write:

```text
Bin/Release/macos-aarch64/Name
Bin/Release/macos-aarch64/libName.dylib
Bin/Release/macos-aarch64/libName.a
```

A native `macos-aarch64` compiler keeps its historical host path and omits the
target component. Cross-produced artifacts are not launched by Rux. Transfer an
executable or its libraries to an Apple Silicon Mac, preserve executable
permissions, and launch it there, for example:

```sh
scp Bin/Release/macos-aarch64/Name apple-silicon-mac:/tmp/Name
ssh apple-silicon-mac 'chmod +x /tmp/Name && /tmp/Name'
```

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

### Apple Silicon runtime acceptance

Byte-level unit tests cover Mach-O layout on every compiler host. Apple Silicon
adds native fixtures for behavior that image inspection cannot establish: a
freestanding exit status, fixed and variadic libSystem calls, assertion and panic
diagnostics, and loading, calling, then unloading an exported dylib. Run all of
them from the repository root with a native AArch64 compiler:

```sh
sh Tests/Native/MacOSAArch64/Verify.sh ./Bin/rux
```

To reproduce the cross-compiler path, pass a thin x86-64 macOS compiler to the
Rosetta wrapper:

```sh
sh Tests/Native/MacOSAArch64/VerifyRosetta.sh /path/to/x86_64/rux
```

Both commands require an underlying Apple Silicon Mac. The scripts preflight the
ARM64 Mach-O header and embedded ad-hoc signature before direct execution; they do
not use an emulator, `codesign`, Xcode command-line tools, or an external
assembler, linker, or archiver.

`rux run` always builds and launches the compiler process's host triple and has
no `--target` option. `rux test --target` requires macOS plus an architecture
reported for either the compiler process or the native OS. Consequently, an
x86-64 compiler running under Rosetta on Apple Silicon may directly test
`macos-aarch64`; a physical Intel Mac may only build/check that target and
transfer it to Apple Silicon for testing.
