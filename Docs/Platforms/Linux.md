# Rux on Linux

This guide covers installing and building Rux on x86-64 or AArch64 Linux. Return to the [main README](../../README.md) for language documentation and project information.

## Installing a Release

Run the per-user installer:

```sh
curl -fsSL https://rux-lang.dev/install.sh | sh
```

The installer selects the native x86-64 or AArch64 release and places `rux` in `~/.local/bin` without requiring root access. Restart your terminal after installation, then verify the compiler:

```sh
rux version
```

Run the installer again to upgrade. The [Linux installer guide](../../Packaging/Linux/README.md) covers version pinning, custom destinations, PATH changes, and removal.

## Building from Source

Rux currently requires Clang 23.1 or newer, CMake 4.4.3 or newer, Ninja 1.13.2 or newer, and a recent Git installation.

On Ubuntu 26.04, install Clang 23 from the LLVM apt repository, CMake from Snap, and the remaining tools from Ubuntu:

```sh
sudo apt-get update
sudo apt-get install -y curl git ninja-build
curl --proto '=https' --tlsv1.2 -fsSLo /tmp/llvm.sh https://apt.llvm.org/llvm.sh
chmod +x /tmp/llvm.sh
sudo /tmp/llvm.sh 23
sudo snap install cmake --classic
```

Clone and build Rux:

```sh
git clone https://github.com/rux-lang/Rux.git
cd Rux
sh Run.sh build
```

The LLVM installer names the compiler `clang++-23`; `Run.sh build` detects it automatically. The command creates a Release build in `Build/` and writes the compiler to `Bin/rux`.

On AArch64, Rux selects the `linux-aarch64` target automatically and compiles it the same way it compiles `linux-x86_64`: instructions are encoded in-process, written into an RCU object, and linked by Rux's own ELF writer. No assembler, C compiler, or external linker is involved on either architecture, so nothing beyond the tools listed above needs to be installed to build Rux programs.

## Native Package Artifacts

An `Executable` package writes `Name`, a `SharedLibrary` writes `libName.so` with its SONAME, and a `StaticLibrary` writes `libName.a` with a GNU archive symbol index. Shared and static libraries can be built but not passed to `rux run`. `SourceLibrary` has no standalone native artifact.

Both backends write ELF objects and images directly, from a host of either architecture: executables, shared libraries with their relocation and PLT tables, and relocatable objects gathered by the common deterministic archive layer into a static library.

For a Debug build, run `sh Run.sh build --configuration Debug`. On other Linux distributions, install equivalent tool versions through the distribution's package manager and pass `--compiler PATH` when Clang is not detected automatically. Run `sh Run.sh` to see every command and option.

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
sh Run.sh test
```

Static analysis is intentionally opt-in because it is slower and requires `clang-tidy` from the same LLVM release:

```sh
sh Run.sh test --clang-tidy
```

Use `sh Run.sh format` to format maintained C++ and Rux sources, or `sh Run.sh format --check` to check them without making changes. Individual workflow steps are also available on their own as `sh Run.sh policy`, `sh Run.sh tidy`, and `sh Run.sh unit`.

Use LLVM 23 for formatting and static analysis, with LF line endings on this platform. Repository verification uses one test worker per available processor; override with `-Jobs N` in PowerShell or `--jobs N` in POSIX shell. See the workflow guide's [build and test throughput](../Workflow.md#build-and-test-throughput) section for compilation caching, optional PCH/ThinLTO, stable metadata, and how to measure a change.
