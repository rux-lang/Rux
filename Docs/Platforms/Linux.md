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

The installer currently selects the x86-64 compatibility asset. On AArch64,
download `rux-linux-aarch64.tar.gz` from the
[latest GitHub release](https://github.com/rux-lang/Rux/releases/latest),
extract it, and place `rux` in a directory on `PATH`.

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

On AArch64, Rux selects the `linux-aarch64` target automatically and compiles it
the same way it compiles `linux-x86_64`: instructions are encoded in-process,
written into an RCU object, and linked by Rux's own ELF writer. No assembler, C
compiler, or external linker is involved on either architecture, so nothing
beyond the tools listed above needs to be installed to build Rux programs.

## Native Package Artifacts

An `Executable` package writes `Name`, a `SharedLibrary` writes `libName.so` with its SONAME, and a `StaticLibrary` writes `libName.a` with a GNU archive symbol index. Shared and static libraries can be built but not passed to `rux run`. `SourceLibrary` has no standalone native artifact.

Both backends write ELF objects and images directly, from a host of either architecture: executables, shared libraries with their relocation and PLT tables, and relocatable objects gathered by the common deterministic archive layer into a static library.

For a Debug build, run `sh Build.sh --configuration Debug`. On other Linux distributions, install equivalent tool versions through the distribution's package manager and pass `--compiler PATH` when Clang is not detected automatically. Run `sh Build.sh --help` to see every option.

## Cross-Compiling and Emulation

`--target <os>-<arch>` builds for a machine other than the host, and writes the
result to its own subdirectory so builds for two targets do not overwrite each
other — `Bin/Release/linux-aarch64/Name` rather than `Bin/Release/Name`:

```sh
./Bin/rux build --target linux-aarch64
```

`rux run` and `rux test` execute what they build. An artifact for this machine
runs directly; one for another architecture runs under a user-mode emulator, so
a cross build can be tested without a second machine:

```sh
sudo apt-get install -y qemu-user
./Bin/rux run --target linux-aarch64
```

Exit codes and output pass through the emulator unchanged, which is what keeps
the `rux test` contract — a test passes by exiting `0` — the same across targets.
`sh Test.sh --target linux-aarch64` runs the whole Rux suite that way; the C++
unit tests, formatting and static analysis are host-side and always run natively.

Two environment variables control the emulator:

| Variable            | Effect                                                                                                                                       |
| ------------------- | -------------------------------------------------------------------------------------------------------------------------------------------- |
| `RUX_EMULATOR`      | The emulator command, replacing the default (`qemu-aarch64` for AArch64). A value naming an existing file is used as written; anything else is a command line whose first word is the program and whose remaining words are passed before the artifact. |
| `RUX_QEMU_SYSROOT`  | A sysroot passed on as `-L`, where a dynamically linked guest program finds its loader and shared libraries. A freestanding program — one importing no shared library — needs none. |

A program that links against glibc needs the matching sysroot, which Debian and
Ubuntu package as `libc6-arm64-cross`:

```sh
sudo apt-get install -y libc6-arm64-cross
RUX_QEMU_SYSROOT=/usr/aarch64-linux-gnu ./Bin/rux test --target linux-aarch64
```

A foreign operating system has no such answer: an emulator supplies an
instruction set, not a kernel, so `rux run --target windows-x86_64` reports that
the artifact has to be run on that system — it is built, not executed.

A Linux host reaches these targets, whichever architecture it runs on:

| Target                                                | Builds | Runs here                      |
| ----------------------------------------------------- | ------ | ------------------------------ |
| `linux-x86_64`, `linux-aarch64`                       | Yes    | Natively, or under `qemu-user` |
| `windows-x86_64`, `macos-x86_64`                      | Yes    | No — foreign operating system  |
| `freebsd-x86_64`, `openbsd-x86_64`, `netbsd-x86_64`   | Yes    | No — foreign operating system  |
| `dragonfly-x86_64`, `illumos-x86_64`                  | Yes    | No — foreign operating system  |
| `macos-aarch64`, `windows-aarch64`, `freebsd-aarch64` | No     | —                              |

The x86-64 back end reaches every supported operating system, because all three
object writers are parameterized for it. The AArch64 back end writes ELF, so it
covers `linux-aarch64` and no other AArch64 system yet: those three targets are
refused with `code generation for '<triple>' is not implemented yet`. AArch64
Mach-O and PE writers are what open them, and are not written yet. `rux check`,
`rux fmt`, `rux lint` and `rux doc` need no back end and work for every named
target.

Nothing in a cross build reaches for a cross toolchain, because there is no
toolchain to reach for: the same encoder, object writer and linker run whichever
machine invokes them, and a `linux-aarch64` image records the loader and library
names it needs without opening a host library to do it. A sysroot is needed to
*run* a cross build, not to produce one, and only when the program imports a
shared library.

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
