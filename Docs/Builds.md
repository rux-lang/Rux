# Package Builds and the Target Matrix

This guide describes `rux build`: profiles, target selection, artifact paths, cross-compilation, and the all-target matrix. It concerns packages compiled by Rux, not the CMake build that produces the Rux compiler itself. For the latter, see the [development workflow](Workflow.md).

Commands use POSIX spelling below. On Windows, replace `./Bin/rux` with `.\Bin\rux.exe`.

## Profiles and Targets

A normal build selects one profile and one target. Debug is the default profile; `--release` selects Release. Debug uses O0 and carries debug information, while Release enables the safe optimization pipeline and omits debug information.

The target defaults to the compiler's host triple. `--target <triple>` selects any of the eight supported targets from any compiler host:

| Canonical target ID | Display name    | Object and image format |
| ------------------- | --------------- | ----------------------- |
| `freebsd-aarch64`   | FreeBSD AArch64 | ELF                     |
| `freebsd-x86_64`    | FreeBSD x86-64  | ELF                     |
| `linux-aarch64`     | Linux AArch64   | ELF                     |
| `linux-x86_64`      | Linux x86-64    | ELF                     |
| `macos-aarch64`     | macOS AArch64   | Mach-O                  |
| `macos-x86_64`      | macOS x86-64    | Mach-O                  |
| `windows-aarch64`   | Windows AArch64 | PE/COFF                 |
| `windows-x86_64`    | Windows x86-64  | PE/COFF                 |

Target IDs are machine-facing values. They use `x86_64` and `aarch64` in `--target`, `#target.triple`, CI artifact IDs, and command examples. Prose, build reports, and output directories use the display spellings macOS, x86-64, and AArch64.

The split runs through every command's output. Progress and outcome lines name the profile and target in display spelling — `Compiling App v0.1.0 (Debug, Windows x86-64)` — because they are prose. A canonical ID appears only in text naming a value you could pass back on the command line, such as the `--target` diagnostics. An alias therefore resolves visibly: `rux build --target freebsd-arm64` reports `FreeBSD AArch64`.

The CLI accepts `x86-64`, `x64`, and `amd64` as x86-64 aliases, and `arm64` as an AArch64 alias. An alias is normalized before compilation and never changes the output directory. For example:

```sh
./Bin/rux build --release --target windows-x86-64
```

writes beneath `Bin/Release/Windows/x86-64/`, exactly as `--target windows-x64` and `--target windows-x86_64` do.

## Ordinary Artifact Paths

`[Build].Output` in `Rux.toml` is an output root and defaults to `Bin`. Every ordinary machine artifact is placed under:

```text
<Output>/<Profile>/<OS>/<Arch>/
```

The OS component is `FreeBSD`, `Linux`, `macOS`, or `Windows`; the architecture component is `x86-64` or `AArch64`.

The rule applies equally to host and cross builds. Given this manifest fragment:

```toml
[Build]
Output = "Dist"
```

these commands produce separate directories:

| Command                                        | Artifact directory              |
| ---------------------------------------------- | ------------------------------- |
| `rux build`                                    | `Dist/Debug/<host-target>/`     |
| `rux build --release`                          | `Dist/Release/<host-target>/`   |
| `rux build --target linux-x86_64`              | `Dist/Debug/Linux/x86-64/`      |
| `rux build --release --target windows-aarch64` | `Dist/Release/Windows/AArch64/` |

Artifact names follow the target OS: Windows executables end in `.exe`; shared libraries use `.dll`, `.so`, or `.dylib`; and static libraries use `.lib` or `.a`. The [manifest reference](Manifest.md#package-manifests) defines package kinds, names, and the raw-root layouts used by tests, generated documentation, and source archives.

## Building the Complete Matrix

`rux build --all` builds every supported target in Debug and Release:

```sh
./Bin/rux build --all
```

The command always attempts these 16 cells sequentially and in this order:

| Order | Profile | Target            | Default artifact directory     |
| ----: | ------- | ----------------- | ------------------------------ |
|     1 | Debug   | `freebsd-aarch64` | `Bin/Debug/FreeBSD/AArch64/`   |
|     2 | Debug   | `freebsd-x86_64`  | `Bin/Debug/FreeBSD/x86-64/`    |
|     3 | Debug   | `linux-aarch64`   | `Bin/Debug/Linux/AArch64/`     |
|     4 | Debug   | `linux-x86_64`    | `Bin/Debug/Linux/x86-64/`      |
|     5 | Debug   | `macos-aarch64`   | `Bin/Debug/macOS/AArch64/`     |
|     6 | Debug   | `macos-x86_64`    | `Bin/Debug/macOS/x86-64/`      |
|     7 | Debug   | `windows-aarch64` | `Bin/Debug/Windows/AArch64/`   |
|     8 | Debug   | `windows-x86_64`  | `Bin/Debug/Windows/x86-64/`    |
|     9 | Release | `freebsd-aarch64` | `Bin/Release/FreeBSD/AArch64/` |
|    10 | Release | `freebsd-x86_64`  | `Bin/Release/FreeBSD/x86-64/`  |
|    11 | Release | `linux-aarch64`   | `Bin/Release/Linux/AArch64/`   |
|    12 | Release | `linux-x86_64`    | `Bin/Release/Linux/x86-64/`    |
|    13 | Release | `macos-aarch64`   | `Bin/Release/macOS/AArch64/`   |
|    14 | Release | `macos-x86_64`    | `Bin/Release/macOS/x86-64/`    |
|    15 | Release | `windows-aarch64` | `Bin/Release/Windows/AArch64/` |
|    16 | Release | `windows-x86_64`  | `Bin/Release/Windows/x86-64/`  |

A configured output root replaces only `Bin` in this table. Because every cell has a profile, an OS, and an architecture component, all 16 directories are distinct and repeated matrix builds use the same paths and order.

### Flags with `--all`

The matrix fixes the target and profile of every cell, so `--all` is incompatible with flags that try to select or alter one of them:

| Flag                      | With `--all` | Reason                                      |
| ------------------------- | ------------ | ------------------------------------------- |
| `--target <triple>`       | No           | The matrix already selects all targets      |
| `--debug`                 | No           | The matrix already includes Debug           |
| `--release`               | No           | The matrix already includes Release         |
| `--emit <kind[,kind...]>` | No           | Inspection output is a single-build feature |
| `--define <name[=value]>` | Yes          | The same definition is applied to each cell |
| `--stats`                 | Yes          | Adds per-cell and aggregate statistics      |
| `--quiet`                 | Yes          | Suppresses reports, but not errors          |
| `--verbose`               | Yes          | Identifies each cell as it starts           |

Global options such as `--manifest` and `--color` remain available. Put a global option before the command, for example:

```sh
./Bin/rux --manifest Packages/App/Rux.toml --color=never build --all --stats --define Checked=true
```

Conflicting flags are usage errors and are rejected before the manifest is loaded or any cell is compiled.

### Failures and Reports

Matrix execution continues after a cell fails. Successful artifacts remain on disk, diagnostics from failed cells are printed, and the command exits nonzero if any of the 16 cells failed.

The normal report lists every cell in matrix order, its outcome, profile, target, elapsed time, and artifact path:

```text
Build matrix for App
Status  Profile  Target              Time      Output
Built   Debug    FreeBSD AArch64     28 ms     Bin\Debug\FreeBSD\AArch64\App
Built   Debug    FreeBSD x86-64      23 ms     Bin\Debug\FreeBSD\x86-64\App
Failed  Debug    Linux AArch64       8 ms      Bin\Debug\Linux\AArch64
```

The target column carries the display name from the [target table](#profiles-and-targets), whose two words are also the two directory components the cell writes to.

Artifact paths are printed relative to the directory holding the manifest; an output root outside that directory keeps its full path. A failed cell reports its output directory rather than an artifact that was never produced. `--stats` adds per-cell and aggregate compiler statistics. `--quiet` prints only errors and suppresses both the matrix table and aggregate statistics. `--verbose` prints a start line for each cell in addition to the final report.

## Build Statistics

`--stats` follows a single build with four sections. It adds to the ordinary report rather than replacing it, so the progress and outcome lines are the same ones a plain `rux build` prints:

```text
Compiling StringBuilder v0.1.0 (Debug, Windows x86-64)
Built StringBuilder (Debug, Windows x86-64) in 21 ms
  Package:  StringBuilder v0.1.0
  Compiler: Rux 0.4.0
  Output:   Bin\Debug\Windows\x86-64\StringBuilder.exe (37 KB)

Time:
  Lexing:                0 ms    0.0%
  Parsing:               0 ms    0.0%
  Analyzing:             2 ms    9.5%
  Lowering to HIR:       1 ms    4.8%
  Lowering to LIR:       1 ms    4.8%
  Emitting RCU objects:  1 ms    4.8%
  Linking:               2 ms    9.5%
  Other:                14 ms   66.7%
  Total:                21 ms  100.0%

Source:
          Local  Dependency  Total
  Files:      1          64     65
  Lines:    119       1,559  1,678
  Tokens:   769       5,551  6,320
  Size:    4 KB       50 KB  54 KB

Optimization:
  Function definitions: 0
  Constants:            0
  Vtables:              0
  Extern declarations:  0
  Declarations pruned:  0
  Estimated IR nodes:   0

Performance:
  Compile speed:     77K LOC/s
  Token throughput:  289.9K tok/s
  Source throughput: 2.41 MB/s
  Peak memory:       11.66 MB
```

Every section is a title at column 0 over indented rows, with the label column sized to that section alone. Rules that hold across all of them:

- **Each fact appears once.** The status line carries the profile, target, and elapsed time, so the rows beneath it carry only what it does not.
- **A total closes the rows it sums.** `Total` ends the time table; `Declarations pruned` ends the four pruning counts it totals. `Estimated IR nodes` is not one of those four, which is why it sits outside the sum and says so in its label.
- **`Other` is measured, not padding.** Phase timers cover lexing through linking; the wall clock also includes resolving dependencies, reading manifests, and filesystem work that no phase measures. `Other` is that difference, so the percentage column always reaches 100%.
- **Phase names match `--verbose`.** Both come from the same list, so a phase named in a progress line or a diagnostic note is spelled the same way here.

`--quiet` suppresses the entire report, statistics included, but never diagnostics.

## Cross-Compilation and Execution

`build` and `check` can select any supported target without an external assembler, compiler, linker, archiver, signer, emulator, or target sysroot. Producing an artifact does not imply the current host can execute it:

- `rux run` is host-only and has no `--target` option.
- `rux test --target` requires the target OS to match the host OS and the target architecture to match either the compiler process or the native OS architecture.
- A foreign artifact should be transferred to a compatible machine for runtime testing.
- Installing an emulator does not make Rux select or launch it.

See the platform guides for native requirements and runtime verification: [FreeBSD](Platforms/FreeBSD.md), [Linux](Platforms/Linux.md), [macOS](Platforms/macOS.md), and [Windows](Platforms/Windows.md). The [CI/CD guide](CI-CD.md) explains which native, cross-compiler, structural, and transferred-artifact paths protect each target.
