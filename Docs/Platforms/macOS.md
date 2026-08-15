# Rux on macOS

This guide covers installing and building Rux on x86-64 and AArch64 macOS. Return to the [main README](../../README.md) for language documentation and project information.

## Installing a Release

Download the `rux-macos-x86_64.tar.gz` or `rux-macos-aarch64.tar.gz` archive matching the Mac from the [latest GitHub release](https://github.com/rux-lang/Rux/releases/latest). Extract it, make `rux` executable, and place it in a directory on `PATH`.

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

On Apple Silicon, Rux selects the `macos-aarch64` target by default. The AArch64 back end reaches `linux-aarch64` through ELF and `windows-aarch64` through PE/COFF, and `macos-aarch64` through Mach-O. Its Mach-O paths write ARM64 object members, deterministic BSD static archives, signed executable images with or without C function imports, and signed shared libraries. All three artifact kinds are available through `rux build --target macos-aarch64` on every supported compiler host. `rux check --target macos-aarch64` selects the Apple target conditions and ARM64 ABI without producing or executing an artifact. Generated images declare macOS 26 as their initial deployment and SDK baseline; supporting older deployment targets is a separate feature.

The `arm64` alias canonicalizes to `aarch64`, so these commands select the same target and output directory:

```sh
./Bin/rux check --target macos-arm64
./Bin/rux build --release --target macos-aarch64
```

## Native Package Artifacts

An `Executable` package writes `Name`, a `SharedLibrary` writes `libName.dylib` with `LC_ID_DYLIB` set to `@rpath/libName.dylib`, and a `StaticLibrary` writes `libName.a` with a BSD archive symbol index. Shared and static libraries can be built but not passed to `rux run`. `SourceLibrary` has no standalone native artifact.

The x86-64 backend writes Mach-O objects and images directly, from a host of either architecture. The AArch64 backend writes relocatable Mach-O objects and the BSD archive used for static libraries, including ARM64 branch, page, page-offset, pointer, and explicit-addend relocations. It links executables with 16 KiB segments, the macOS 26 build-version command, and an in-process ad-hoc signature. Every AArch64 executable is dynamic and position-independent — the macOS kernel refuses a static arm64 image outright and refuses a dyld-linked one that is not marked `MH_PIE` — so even a program with no imports carries `LC_LOAD_DYLINKER`, `LC_MAIN`, and a `libSystem` dependency. Because the loader slides such an image, constant data holding absolute pointers moves out of read-only `__TEXT` into a writable `__DATA_CONST` segment, marked `SG_READ_ONLY`, that dyld rebases and then re-protects read-only — current dyld refuses the segment without the flag. Executables with C imports additionally use eagerly bound non-lazy pointers and Apple ARM64 X16 symbol stubs. Shared libraries use the same stubs for external calls, export public code and data through dyld metadata, rebase image-local pointers, omit executable entry commands, and carry deterministic `@rpath/libName.dylib` identities. Imported data remains unavailable until GOT-aware lowering is implemented.

Every Release build adds the target operating system and architecture below the configured output directory. With the default `Output = "Bin"`, the three package kinds therefore write:

```text
Bin/Release/macOS/AArch64/Name
Bin/Release/macOS/AArch64/libName.dylib
Bin/Release/macOS/AArch64/libName.a
```

The path is identical on a native `macos-aarch64` compiler. Cross-produced artifacts are not launched by Rux. Transfer an executable or its libraries to an Apple Silicon Mac, preserve executable permissions, and launch it there, for example:

```sh
scp Bin/Release/macOS/AArch64/Name apple-silicon-mac:/tmp/Name
ssh apple-silicon-mac 'chmod +x /tmp/Name && /tmp/Name'
```

`rux build --all` produces both macOS targets as part of its 16 Debug/Release cells; see the [matrix path and flag rules](../Builds.md#building-the-complete-matrix).

## Verifying the Build

Run the compiler:

```sh
./Bin/rux version
file ./Bin/rux
```

On Apple Silicon, `file` should report a `Mach-O 64-bit executable arm64`. Programs built by `./Bin/rux build` should report the same architecture.

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

Byte-level unit tests cover Mach-O layout on every compiler host. Apple Silicon adds native fixtures for behavior that image inspection cannot establish: an import-free exit status, fixed and variadic libSystem calls, assertion and panic diagnostics, and loading, calling, then unloading an exported dylib. Run all of them from the repository root with a native AArch64 compiler:

```sh
sh Tests/Native/MacOSAArch64/Verify.sh ./Bin/rux
```

To reproduce the cross-compiler path, pass a thin x86-64 macOS compiler to the Rosetta wrapper:

```sh
sh Tests/Native/MacOSAArch64/VerifyRosetta.sh /path/to/x86_64/rux
```

Both commands require an underlying Apple Silicon Mac. The scripts preflight the ARM64 Mach-O header and embedded ad-hoc signature before direct execution; they do not use an emulator, `codesign`, Xcode command-line tools, or an external assembler, linker, or archiver.

Linux and Windows CI use the non-executing cross verifier instead:

```powershell
./Tests/Native/MacOSAArch64/VerifyCross.ps1 -Rux ./Bin/rux.exe # omit .exe on Linux
```

It builds the signed executable and dylib fixtures twice, checks deterministic bytes, parses their ARM64 Mach-O headers and load commands, and recomputes every CodeDirectory SHA-256 page hash. It never launches the foreign artifacts.

`rux run` always builds and launches the compiler process's host triple and has no `--target` option. `rux test --target` requires macOS plus an architecture reported for either the compiler process or the native OS. Consequently, an x86-64 compiler running under Rosetta on Apple Silicon may directly test `macos-aarch64`; a physical Intel Mac may only build/check that target and transfer it to Apple Silicon for testing.

The required `macOS.yml` workflow runs the complete compiler, unit, workspace, and native fixture suites on `macos-26`, then repeats target tests and fixtures with the downloaded x86-64 compiler under Rosetta. The release workflow runs the same native Apple Silicon acceptance before publishing `rux-macos-aarch64.tar.gz`. GitHub's `macos-26` runner is the normal acceptance environment; EC2 Mac is reserved for prolonged debugging, crash capture, or demonstrated GitHub-runner instability and is not required for development, merging, or releases.
