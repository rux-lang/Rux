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

On AArch64, Rux selects the `freebsd-aarch64` target automatically. The same
target is available explicitly to `build` and `check` on every supported host;
`freebsd-arm64` is accepted as an alias and canonicalized to
`freebsd-aarch64` before target conditions, reports, and output paths are
selected.

## Native Package Artifacts

An `Executable` package writes `Name`, a `SharedLibrary` writes `libName.so` with its SONAME, and a `StaticLibrary` writes `libName.a` with a GNU archive symbol index. Shared and static libraries can be built but not passed to `rux run`. `SourceLibrary` has no standalone native artifact.

Both back ends write FreeBSD ELF objects and images directly, from a host of
either architecture. FreeBSD AArch64 executables use `/libexec/ld-elf.so.1`
and `libc.so.7` when they import functions. Shared libraries omit executable
entry and interpreter state, and static libraries contain FreeBSD AArch64
relocatable ELF members. No target sysroot, assembler, compiler, linker, or
archiver is consulted.

## Cross-Compiling

Use the canonical target or its `arm64` compatibility alias:

```sh
./Bin/rux check --target freebsd-aarch64
./Bin/rux build --release --target freebsd-arm64
```

A foreign build uses a target-separated directory. With the default output,
the example above writes `Bin/Release/freebsd-aarch64/Name`; a native
`freebsd-aarch64` build keeps the historical `Bin/Release/Name` path. Build
reports always identify the canonical `freebsd-aarch64` target.

`rux run` remains host-only and has no `--target` option. `rux test --target
freebsd-aarch64` runs only on FreeBSD when AArch64 is either the compiler
process architecture or the native OS architecture. On every other host it
refuses before compilation and recommends cross-building, transferring the
artifact, and testing it on a native FreeBSD AArch64 machine; Rux never selects
an emulator.

To reproduce the CI transfer boundary, start in a Rux checkout on an x86-64
FreeBSD machine and build a sealed target-only payload:

```sh
sh Tests/Native/FreeBSDAArch64/BuildTransfer.sh ./Bin/rux /tmp/FreeBSDAArch64Payload
tar -C /tmp -czf /tmp/FreeBSDAArch64Payload.tar.gz FreeBSDAArch64Payload
scp /tmp/FreeBSDAArch64Payload.tar.gz arm64-host:/tmp/
```

The payload contains no compiler or sysroot. In a checkout on the AArch64
FreeBSD machine, unpack and verify it directly:

```sh
tar -C /tmp -xzf /tmp/FreeBSDAArch64Payload.tar.gz
sh Tests/Native/FreeBSDAArch64/VerifyTransfer.sh /tmp/FreeBSDAArch64Payload
```

The verifier restores the declared executable modes, checks SHA-256 hashes and
FreeBSD AArch64 ELF identity, then covers freestanding exit, fixed and variadic
libc calls, assertion diagnostics, raw BSD syscalls, and shared-library
load/call/unload. It does not invoke a compiler, ELF tool, or emulator.

For a Debug build, run `sh Build.sh --configuration Debug`. Run `sh Build.sh --help` to see every option.

## Verifying the Build

Run the compiler:

```sh
./Bin/rux version
```

On a native FreeBSD AArch64 machine, run the focused runtime acceptance
fixtures directly. They cover freestanding exit, fixed and variadic libc calls,
assertion and panic diagnostics, raw BSD syscalls, and shared-library loading:

```sh
sh Tests/Native/FreeBSDAArch64/Verify.sh ./Bin/rux
```

The verifier preflights every ELF image using its repository-owned byte reader
before launching it. It invokes no external compiler, linker, ELF inspection
tool, or emulator.

Run the complete repository verification workflow:

```sh
sh Test.sh
```

Static analysis is intentionally opt-in because it is slower and requires `clang-tidy` from the LLVM 22 package:

```sh
sh Test.sh --clang-tidy
```

Use `sh Format.sh` to format maintained C++ and Rux sources, or `sh Format.sh --check` to check them without making changes.
