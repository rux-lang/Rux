# Rux on FreeBSD

This guide covers installing and building Rux on x86-64 and AArch64 FreeBSD 15.1, the CI and release baseline. Return to the [main README](../../README.md) for language documentation and project information.

## Installing a Release

Download the `rux-freebsd-x86_64.tar.gz` or `rux-freebsd-aarch64.tar.gz` archive matching the host from the [latest GitHub release](https://github.com/rux-lang/Rux/releases/latest). Extract it, make `rux` executable, and place it in a directory on `PATH`.

## Building from Source

Rux currently requires Clang 23.1 or newer, CMake 4.4.3 or newer, Ninja 1.13.2 or newer, and a recent Git installation. Install the compiler, Ninja, and bootstrap CMake through `pkg`:

```sh
sudo pkg install -y llvm23 cmake ninja git
```

Clone Rux, build the pinned CMake 4.4.3 in a private directory when the package is older, and build the compiler:

```sh
git clone https://github.com/rux-lang/Rux.git
cd Rux
sh .github/scripts/Install-CMake-FreeBSD.sh BuildCache/CMake
export PATH="$PWD/BuildCache/CMake/bin:$PATH"
sh Run.sh build
```

The FreeBSD package names the compiler `clang++23`; `Run.sh build` detects it automatically. The command creates a Release build in `Build/` and writes the compiler to `Bin/rux`.

On AArch64, Rux selects the `freebsd-aarch64` target automatically. The same target is available explicitly to `build` and `check` on every supported host; `freebsd-arm64` is accepted as an alias and canonicalized to `freebsd-aarch64` before target conditions, reports, and output paths are selected.

## Native Package Artifacts

An `Executable` package writes `Name`, a `SharedLibrary` writes `libName.so` with its SONAME, and a `StaticLibrary` writes `libName.a` with a GNU archive symbol index. Shared and static libraries can be built but not passed to `rux run`. `SourceLibrary` has no standalone native artifact.

The command boundary is the same for all three native artifact kinds:

| Operation        | `freebsd-aarch64` behavior                                                                  |
| ---------------- | ------------------------------------------------------------------------------------------- |
| `check --target` | Analyzes FreeBSD conditions and the AAPCS64 ABI on every supported host; writes no artifact |
| `build --target` | Writes the manifest-selected executable, shared library, or static library                  |
| `run`            | Builds and launches only the host target; it has no `--target` option                       |
| `test --target`  | Builds and launches only on FreeBSD when the compiler process or native OS is AArch64       |

The restrictions are execution policy, not output limitations: cross-builds can produce all three artifact kinds even when the host cannot launch any of them.

Both back ends write FreeBSD ELF objects and images directly, from a host of either architecture. FreeBSD AArch64 executables use `/libexec/ld-elf.so.1` and `libc.so.7` when they import functions. Shared libraries omit executable entry and interpreter state, and static libraries contain FreeBSD AArch64 relocatable ELF members. No target sysroot, assembler, compiler, linker, or archiver is consulted.

## Cross-Compiling

Use the canonical target or its `arm64` compatibility alias:

```sh
./Bin/rux check --target freebsd-aarch64
./Bin/rux build --release --target freebsd-arm64
```

Every build uses a target-separated directory. With the default output, the example above writes `Bin/Release/FreeBSD/AArch64/Name` on both native and foreign compiler hosts. Build reports always identify the resolved target as FreeBSD AArch64, so the `arm64` alias is visibly normalized.

`rux build --all` also produces both FreeBSD targets in Debug and Release; see the [matrix path and flag rules](../Builds.md#building-the-complete-matrix).

`rux run` remains host-only and has no `--target` option. `rux test --target freebsd-aarch64` runs only on FreeBSD when AArch64 is either the compiler process architecture or the native OS architecture. On every other host it refuses before compilation and recommends cross-building, transferring the artifact, and testing it on a native FreeBSD AArch64 machine; Rux never selects an emulator.

To reproduce the CI transfer boundary, start in a Rux checkout on an x86-64 FreeBSD machine and build a sealed target-only payload:

```sh
sh Tests/Native/FreeBSDAArch64/BuildTransfer.sh ./Bin/rux /tmp/FreeBSDAArch64Payload
tar -C /tmp -czf /tmp/FreeBSDAArch64Payload.tar.gz FreeBSDAArch64Payload
scp /tmp/FreeBSDAArch64Payload.tar.gz arm64-host:/tmp/
```

The payload contains no compiler or sysroot. In a checkout on the AArch64 FreeBSD machine, unpack and verify it directly:

```sh
tar -C /tmp -xzf /tmp/FreeBSDAArch64Payload.tar.gz
sh Tests/Native/FreeBSDAArch64/VerifyTransfer.sh /tmp/FreeBSDAArch64Payload
```

The verifier restores the declared executable modes, checks SHA-256 hashes and FreeBSD AArch64 ELF identity, then covers freestanding exit, fixed and variadic libc calls, assertion diagnostics, raw BSD syscalls, and shared-library load/call/unload. It does not invoke a compiler, ELF tool, or emulator.

CI applies both acceptance boundaries before publishing `rux-freebsd-aarch64`: a native AArch64 compiler runs the complete ordinary Rux suite and focused fixtures, and an x86-64 compiler builds a target-only payload that a fresh AArch64 VM verifies and executes. Skipping either path blocks the aggregate FreeBSD acceptance check, which is a direct dependency of the release publish job.

For a Debug build, run `sh Run.sh build --configuration Debug`. Run `sh Run.sh` to see every command and option.

## Verifying the Build

Run the compiler:

```sh
./Bin/rux version
```

On a native FreeBSD AArch64 machine, run the focused runtime acceptance fixtures directly. They cover freestanding exit, fixed and variadic libc calls, assertion and panic diagnostics, raw BSD syscalls, and shared-library loading:

```sh
sh Tests/Native/FreeBSDAArch64/Verify.sh ./Bin/rux
```

The verifier preflights every ELF image using its repository-owned byte reader before launching it. It invokes no external compiler, linker, ELF inspection tool, or emulator.

Run the complete repository verification workflow:

```sh
sh Run.sh test
```

Static analysis is intentionally opt-in because it is slower and requires `clang-tidy` from the LLVM 23 package:

```sh
sh Run.sh test --clang-tidy
```

Use `sh Run.sh format` to format maintained C++ and Rux sources, or `sh Run.sh format --check` to check them without making changes. Individual workflow steps are also available on their own as `sh Run.sh policy`, `sh Run.sh tidy`, and `sh Run.sh unit`.

Use LLVM 23 for formatting and static analysis, with LF line endings on this platform. Repository verification uses up to four available test workers; override with `-Jobs N` in PowerShell or `--jobs N` in POSIX shell. See [Compiler Build Performance](../CompilerPerformance.md) for compilation caching, optional PCH/ThinLTO, stable metadata, and measurement commands.
