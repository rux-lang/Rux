# Rux on Linux

This guide covers installing and building Rux on x86-64 or AArch64 Linux. Return to the [main README](../../README.md) for language documentation and project information.

## Installing a Release

Run the per-user installer:

```sh
curl -fsSL https://rux-lang.dev/install.sh | sh
```

The installer places `rux` in `~/.local/bin` without requiring root access. Open a new terminal after installation, then verify the compiler:

```sh
rux version
```

Run the installer again to upgrade. The [Linux installer guide](../../Packaging/Linux/README.md) covers version pinning, custom destinations, PATH changes, and removal.

The installer currently selects the x86-64 compatibility asset. On AArch64, download `rux-linux-aarch64.tar.gz` from the [latest GitHub release](https://github.com/rux-lang/Rux/releases/latest), extract it, and place `rux` in a directory on `PATH`.

## Building from Source

Rux currently requires Clang 22.1 or newer, CMake 3.30 or newer, Ninja 1.11 or newer, and a recent Git installation.

On Ubuntu 26.04, install Clang 22 from the LLVM apt repository, CMake from Snap, and the remaining tools from Ubuntu:

```sh
sudo apt-get update
sudo apt-get install -y curl git ninja-build
curl --proto '=https' --tlsv1.2 -fsSLo /tmp/llvm.sh https://apt.llvm.org/llvm.sh
chmod +x /tmp/llvm.sh
sudo /tmp/llvm.sh 22
sudo snap install cmake --classic
```

Clone and build Rux:

```sh
git clone https://github.com/rux-lang/Rux.git
cd Rux
sh Build.sh
```

The LLVM installer names the compiler `clang++-22`; `Build.sh` detects it automatically. The script creates a Release build in `Build/` and writes the compiler to `Bin/rux`.

On AArch64, Rux selects the `linux-aarch64` target automatically and compiles it the same way it compiles `linux-x86_64`: instructions are encoded in-process, written into an RCU object, and linked by Rux's own ELF writer. No assembler, C compiler, or external linker is involved on either architecture, so nothing beyond the tools listed above needs to be installed to build Rux programs.

## Native Package Artifacts

An `Executable` package writes `Name`, a `SharedLibrary` writes `libName.so` with its SONAME, and a `StaticLibrary` writes `libName.a` with a GNU archive symbol index. Shared and static libraries can be built but not passed to `rux run`. `SourceLibrary` has no standalone native artifact.

Both backends write ELF objects and images directly, from a host of either architecture: executables, shared libraries with their relocation and PLT tables, and relocatable objects gathered by the common deterministic archive layer into a static library.

For a Debug build, run `sh Build.sh --configuration Debug`. On other Linux distributions, install equivalent tool versions through the distribution's package manager and pass `--compiler PATH` when Clang is not detected automatically. Run `sh Build.sh --help` to see every option.

## Cross-Compiling and Testing

`--target <os>-<arch>` builds for a machine other than the host. Every ordinary artifact has its own profile, operating-system, and architecture directory, so builds cannot overwrite each other — for example, `Bin/Release/Linux/AArch64/Name`:

```sh
./Bin/rux build --target linux-aarch64
```

`rux build --all` produces both Linux targets as part of its 16 Debug/Release cells; see the [matrix path and flag rules](../Builds.md#building-the-complete-matrix).

`rux run` is host-only and has no `--target` option. Use `build` or `check` to select a cross target, then transfer the artifact to a native target machine:

```sh
./Bin/rux check --target linux-aarch64
./Bin/rux build --release --target linux-aarch64
```

`rux test --target` is narrower than cross-compilation. It runs only when the target OS matches the host OS and the target architecture equals either the compiler process architecture or the native OS architecture. On a physical x86-64 Linux host, `rux test --target linux-aarch64` therefore fails before compiling the suite and recommends build/check plus native testing. Installing an instruction-set emulator does not change that decision.

A Linux host reaches these targets, whichever architecture it runs on:

| Target                                                               | Builds | Target tests on this host                        |
| -------------------------------------------------------------------- | ------ | ------------------------------------------------ |
| Host Linux architecture                                              | Yes    | Yes                                              |
| Other Linux architecture                                             | Yes    | Only when reported as the native OS architecture |
| `windows-x86_64`, `windows-aarch64`, `macos-x86_64`, `macos-aarch64` | Yes    | No — foreign operating system                    |
| `freebsd-x86_64`, `freebsd-aarch64`                                  | Yes    | No — foreign operating system                    |

Both back ends reach every supported triple, because all three object writers are parameterized for both architectures: `freebsd-*` and `linux-*` through ELF, `windows-*` through PE/COFF, and `macos-*` through Mach-O. `rux check`, `rux fmt`, `rux lint` and `rux doc` need no back end and work for every named target.

Nothing in a cross build reaches for a cross toolchain, because there is no toolchain to reach for: the same encoder, object writer and linker run whichever machine invokes them, and a `linux-aarch64` image records the loader and library names it needs without opening a host library to do it.

A Linux host can likewise build FreeBSD AArch64 executables, shared libraries, and static libraries. Release output is written below `Bin/Release/FreeBSD/AArch64/`; transfer those artifacts to native FreeBSD for execution because `run` is host-only and target tests reject a foreign OS.

## Verifying the Build

Run the compiler:

```sh
./Bin/rux version
```

Run the complete repository verification workflow:

```sh
sh Test.sh
```

Static analysis is intentionally opt-in because it is slower and requires `clang-tidy` from the same LLVM release:

```sh
sh Test.sh --clang-tidy
```

Use `sh Format.sh` to format maintained C++ and Rux sources, or `sh Format.sh --check` to check them without making changes.
