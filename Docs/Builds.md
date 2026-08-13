# Package Builds and the Target Matrix

This guide describes `rux build`: profiles, target selection, artifact paths, cross-compilation, and the all-target matrix. It concerns packages compiled by Rux, not the CMake build that produces the Rux compiler itself. For the latter, see the [development workflow](Workflow.md).

Commands use POSIX spelling below. On Windows, replace `./Bin/rux` with `.\Bin\rux.exe`.

## Profiles and Targets

A normal build selects one profile and one target. Debug is the default profile; `--release` selects Release. Debug uses O0 and carries debug information, while Release enables the safe optimization pipeline and omits debug information.

The target defaults to the compiler's host triple. `--target <triple>` selects any of the eight supported targets from any compiler host:

| Canonical target ID   | Display name          | Object and image format |
| --------------------- | --------------------- | ----------------------- |
| `freebsd-x86_64`      | FreeBSD x86-64        | ELF                     |
| `freebsd-aarch64`     | FreeBSD AArch64       | ELF                     |
| `linux-x86_64`        | Linux x86-64          | ELF                     |
| `linux-aarch64`       | Linux AArch64         | ELF                     |
| `macos-x86_64`        | macOS x86-64          | Mach-O                  |
| `macos-aarch64`       | macOS AArch64         | Mach-O                  |
| `windows-x86_64`      | Windows x86-64        | PE/COFF                 |
| `windows-aarch64`     | Windows AArch64       | PE/COFF                 |

Target IDs are machine-facing values. They use `x86_64` and `aarch64`, including in output directories, `#target.triple`, CI artifact IDs, and command examples. Prose and reports use the display spellings x86-64 and AArch64.

The CLI accepts `x86-64`, `x64`, and `amd64` as x86-64 aliases, and `arm64` as an AArch64 alias. An alias is normalized before compilation and never changes the output directory. For example:

```sh
./Bin/rux build --release --target windows-x86-64
```

writes beneath `Bin/Release/windows-x86_64/`, not a directory named after the alias.

## Ordinary Artifact Paths

`[Build].Output` in `Rux.toml` is an output root and defaults to `Bin`. Every ordinary machine artifact is placed under:

```text
<Output>/<Profile>/<canonical-target>/
```

The rule applies equally to host and cross builds. Given this manifest fragment:

```toml
[Build]
Output = "Dist"
```

these commands produce separate directories:

| Command                                                   | Artifact directory                  |
| --------------------------------------------------------- | ----------------------------------- |
| `rux build`                                               | `Dist/Debug/<host-target>/`         |
| `rux build --release`                                     | `Dist/Release/<host-target>/`       |
| `rux build --target linux-x86_64`                         | `Dist/Debug/linux-x86_64/`          |
| `rux build --release --target windows-aarch64`            | `Dist/Release/windows-aarch64/`     |

Artifact names follow the target OS: Windows executables end in `.exe`; shared libraries use `.dll`, `.so`, or `.dylib`; and static libraries use `.lib` or `.a`. The [manifest reference](Manifest.md#package-manifests) defines package kinds, names, and the raw-root layouts used by tests, generated documentation, and source archives.

## Building the Complete Matrix

`rux build --all` builds every supported target in Debug and Release:

```sh
./Bin/rux build --all
```

The command always attempts these 16 cells sequentially and in this order:

| Order | Profile | Target                | Default artifact directory             |
| ----: | ------- | --------------------- | -------------------------------------- |
|     1 | Debug   | `freebsd-x86_64`      | `Bin/Debug/freebsd-x86_64/`            |
|     2 | Debug   | `freebsd-aarch64`     | `Bin/Debug/freebsd-aarch64/`           |
|     3 | Debug   | `linux-x86_64`        | `Bin/Debug/linux-x86_64/`              |
|     4 | Debug   | `linux-aarch64`       | `Bin/Debug/linux-aarch64/`             |
|     5 | Debug   | `macos-x86_64`        | `Bin/Debug/macos-x86_64/`              |
|     6 | Debug   | `macos-aarch64`       | `Bin/Debug/macos-aarch64/`             |
|     7 | Debug   | `windows-x86_64`      | `Bin/Debug/windows-x86_64/`            |
|     8 | Debug   | `windows-aarch64`     | `Bin/Debug/windows-aarch64/`           |
|     9 | Release | `freebsd-x86_64`      | `Bin/Release/freebsd-x86_64/`          |
|    10 | Release | `freebsd-aarch64`     | `Bin/Release/freebsd-aarch64/`         |
|    11 | Release | `linux-x86_64`        | `Bin/Release/linux-x86_64/`            |
|    12 | Release | `linux-aarch64`       | `Bin/Release/linux-aarch64/`           |
|    13 | Release | `macos-x86_64`        | `Bin/Release/macos-x86_64/`            |
|    14 | Release | `macos-aarch64`       | `Bin/Release/macos-aarch64/`           |
|    15 | Release | `windows-x86_64`      | `Bin/Release/windows-x86_64/`          |
|    16 | Release | `windows-aarch64`     | `Bin/Release/windows-aarch64/`         |

A configured output root replaces only `Bin` in this table. Because every cell has both a profile and canonical target component, all 16 directories are distinct and repeated matrix builds use the same paths and order.

### Flags with `--all`

The matrix fixes the target and profile of every cell, so `--all` is incompatible with flags that try to select or alter one of them:

| Flag                         | With `--all` | Reason                                      |
| ---------------------------- | ------------ | ------------------------------------------- |
| `--target <triple>`          | No           | The matrix already selects all targets      |
| `--debug`                    | No           | The matrix already includes Debug           |
| `--release`                  | No           | The matrix already includes Release         |
| `--emit <kind[,kind...]>`    | No           | Inspection output is a single-build feature |
| `--define <name[=value]>`    | Yes          | The same definition is applied to each cell |
| `--stats`                    | Yes          | Adds per-cell and aggregate statistics      |
| `--quiet`                    | Yes          | Suppresses reports, but not errors           |
| `--verbose`                  | Yes          | Identifies each cell as it starts            |

Global options such as `--manifest` and `--color` remain available. Put a global option before the command, for example:

```sh
./Bin/rux --manifest Packages/App/Rux.toml --color=never build --all --stats --define Checked=true
```

Conflicting flags are usage errors and are rejected before the manifest is loaded or any cell is compiled.

### Failures and Reports

Matrix execution continues after a cell fails. Successful artifacts remain on disk, diagnostics from failed cells are printed, and the command exits nonzero if any of the 16 cells failed.

The normal report lists every cell in matrix order, its outcome, target, profile, elapsed time, and artifact path. `--stats` adds per-cell and aggregate compiler statistics. `--quiet` prints only errors and suppresses both the matrix table and aggregate statistics. `--verbose` prints a start line for each cell in addition to the final report.

## Cross-Compilation and Execution

`build` and `check` can select any supported target without an external assembler, compiler, linker, archiver, signer, emulator, or target sysroot. Producing an artifact does not imply the current host can execute it:

- `rux run` is host-only and has no `--target` option.
- `rux test --target` requires the target OS to match the host OS and the target architecture to match either the compiler process or the native OS architecture.
- A foreign artifact should be transferred to a compatible machine for runtime testing.
- Installing an emulator does not make Rux select or launch it.

See the platform guides for native requirements and runtime verification: [FreeBSD](Platforms/FreeBSD.md), [Linux](Platforms/Linux.md), [macOS](Platforms/macOS.md), and [Windows](Platforms/Windows.md). The [CI/CD guide](CI-CD.md) explains which native, cross-compiler, structural, and transferred-artifact paths protect each target.
