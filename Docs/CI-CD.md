# CI/CD Flow

Continuous integration runs on every push and pull request to `main` and `dev`, building and testing Rux across every supported operating system. Continuous delivery (publishing binaries) is covered separately in the [Release Pipeline](Release.md).

## Per-OS Build and Test Workflows

Each supported platform has its own workflow under [`.github/workflows/`](../.github/workflows/):

| Workflow      | Platform                        | Runner            | Toolchain install         |
| ------------- | ------------------------------- | ----------------- | ------------------------- |
| `FreeBSD.yml` | FreeBSD 14.4 x86-64 and AArch64 | QEMU VM on Ubuntu | `pkg llvm22`              |
| `Linux.yml`   | Ubuntu 24.04 x86-64 and AArch64, plus an x86-64 → AArch64 build/check job | GitHub-hosted     | `apt.llvm.org` → Clang 22 |
| `macOS.yml`   | macOS 26 x86-64 and AArch64     | GitHub-hosted     | Homebrew `llvm@22`        |
| `Windows.yml` | Windows 2025 and Windows 11 ARM, plus an x86-64 compiler → AArch64 target cross job | GitHub-hosted | Runner's bundled Clang |

Their status is shown by the badges at the top of the [README](../README.md).

Two repository-policy workflows run alongside the per-OS matrix:

- **`CodeQuality.yml`** — repository-wide checks: the platform-isolation guard (`Tests/Policy/PlatformIsolation/Check.sh`, which fails when OS APIs like `getenv`/`<windows.h>`/`fork` are used outside `Compiler/System/`), the external-toolchain guard (`Tests/Policy/NoExternalToolchain/Check.sh`, described below), a `clang-format-22 --dry-run -Werror` pass, and parallel `clang-tidy-22` static analysis over the maintained C++ translation units in CMake's compilation database.
- **`PullRequestPolicy.yml`** — rejects pull requests targeting `main` and directs contributors to the `dev` integration branch.

### The External-Toolchain Guard

`Tests/Policy/NoExternalToolchain/Check.sh` protects the property the whole compiler is built around: Rux encodes its own machine code, writes its own object files and links its own executables, so no part of it may shell out to a build tool. The check greps `Compiler/` for two things and fails on either —

- a string literal naming an assembler, C compiler, linker or archiver, under any spelling a real one carries: a path (`/usr/bin/clang`), a cross prefix (`aarch64-linux-gnu-gcc`), a version suffix (`clang-22`) or a Windows extension (`link.exe`). Two-letter names like `as`, `cc` and `ld` are assembler mnemonics and register names throughout `CodeGen/`, so they count only when the literal also carries a directory or an extension;
- a call to `System::RunInherited` or `System::RunCaptured` — the two entry points every process launch goes through — from outside `Compiler/System/`.

No file is allowed to name a toolchain program. The second check has a short allowlist at the top of the script, and a file joins it only with a reason written beside it: running a program is not the same as building one, so `Cli/CmdRun.cpp` and `Cli/CmdTest.cpp` are its two permanent entries, directly executing the host artifact or a directly executable same-OS target test.

The guard runs as its own job in `CodeQuality.yml` and as the first step of `Test.sh` and `Test.ps1`, beside the platform-isolation check.

Every platform build configures with `-DRUX_BUILD_TESTS=ON` and runs the C++ unit tests (doctest via `ctest`) before uploading the binary artifact; see [Development Workflow](Workflow.md) for the test layout.
The Linux test job additionally checks every maintained Rux source with `rux fmt --check`, complementing the C++ formatting job without compiling the compiler a second time in `CodeQuality.yml`.

## Triggers

```yaml
on:
  push:
    branches: [main, dev]
  pull_request:
    branches: [main, dev]
```

Superseded runs on the same ref are cancelled automatically (`concurrency: cancel-in-progress`), so only the latest commit on a branch or PR keeps running.

## What Each Run Does

Each per-OS validation workflow has an x86-64/AArch64 matrix with two stages —
**Build**, then **Test** (`needs: build`). Splitting them means each native
binary is compiled once, uploaded as an architecture-labelled artifact, then
downloaded by the matching test job. Using `Linux.yml` as the reference shape:

1. **Build job**
   - Check out the repo.
   - Install Clang 22, plus pinned CMake and Ninja versions.
   - Configure and build Release (Clang jobs add `-DRUX_WERROR=ON`, so warnings fail the build):
     ```sh
     cmake -S . -B Build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++-22 -DRUX_WERROR=ON -DRUX_BUILD_TESTS=ON
     cmake --build Build --config Release --parallel
     ctest --test-dir Build --output-on-failure -C Release
     ```
   - Upload the `rux` binary as an artifact.
2. **Test job** (`needs: build`)
   - Download the built binary and restore its executable bit.
   - On Linux, verify Rux formatting across every package and test manifest.
   - Run `rux check`, `rux lint`, and `rux test --release` from the repo root. Workspace mode discovers every language and package test below `Tests/`, resolves first-party dependencies locally, and disables registry fallback.

`Linux.yml` and `Windows.yml` each add a third job to those two.

3. **Linux cross job** (`needs: build`, `ubuntu-24.04`)
   - Download the x86-64 binary built by the build job.
   - Run `rux check --target linux-aarch64`, build a representative AArch64 executable, and inspect its repository-produced ELF header for `EM_AARCH64`. The x86-64 host never launches the output. `rux lint` takes no target and is not repeated here.

4. **Windows cross job** (`needs: build`, `windows-11-arm`)
   - Download the x86-64 Windows compiler and run it through Windows-on-ARM emulation. No compiler toolchain is installed in this job.
   - Run `rux check --target windows-aarch64` and `rux test --release --target windows-aarch64`. The emulated compiler detects that the underlying OS is AArch64, so the programs it produces execute natively.
   - Build and run the dedicated executable exit-code and DLL load/call/unload fixtures. These protect process launch, PE entry, imports, exports, and import-library handling beyond the workspace suite.

Neither cross job installs a C compiler, assembler, linker, or archiver: Rux encodes and links both AArch64 formats itself. The Linux job is build/check-only, while the Windows job runs on an AArch64 OS and can execute its output directly. The native Windows AArch64 job runs the complete language/package suite and the same native fixtures, and the release workflow repeats that validation before packaging `rux-windows-aarch64.zip`. macOS and FreeBSD AArch64 compilers still cannot emit native AArch64 images because those image writers are not implemented.

### Platform-Specific Quirks

The native-runner workflows differ only in how the compiler is obtained; the emulated ones differ in _where the whole job runs_:

- **Ubuntu** — installs Clang 22 from `apt.llvm.org` and builds with `clang++-22` on `ubuntu-24.04` (x86-64) and `ubuntu-24.04-arm` (AArch64). Clang is the host C++ compiler that builds `rux`, and nothing else: the AArch64 test and cross jobs run the compiler's own back end.
- **Windows** — uses the runner's bundled Clang on `windows-2025` (x86-64) and `windows-11-arm` (AArch64). Before native builds, `.github/scripts/Enter-VsDevEnv.ps1` locates Visual Studio with `vswhere` and imports the matching x86-64 or ARM64 toolset, and Clang is given the explicit MSVC target triple. The cross job needs neither setup step: it downloads the already-built x86-64 compiler and relies on Windows-on-ARM only to run that compiler.
- **macOS** — Apple Clang lags upstream and lacks full C++26 support, so the workflow installs LLVM `llvm@22` from Homebrew and points `CMAKE_CXX_COMPILER` at the Homebrew `clang++`.
- **FreeBSD** — GitHub has no native FreeBSD runner, so each job boots an x86-64 or AArch64 FreeBSD 14.4 QEMU VM via `vmactions/freebsd-vm` on an Ubuntu host. Because Build and Test are separate jobs, each boots a _fresh_ VM; the Test VM installs the Clang runtime libraries needed by the prebuilt binary.

## Required Checks

The following must pass before a PR can merge (configured in branch protection — see [Branch Architecture](Branches.md)):

- **`CodeQuality.yml`** (platform isolation, no external toolchain, formatting, and static analysis)
- **`Linux.yml`** (Ubuntu 24.04 x86-64 and AArch64, and the AArch64 cross job)
- **`Windows.yml`** (Windows x86-64 and AArch64, and the x86-64 compiler → AArch64 target cross job)

The remaining workflows — `macOS.yml` and `FreeBSD.yml` — run on every push and PR and report status, but are **informational**: they broaden platform coverage without blocking merges, since non-required platforms can be slower or occasionally flaky. A red informational check is still worth investigating before merging.

## Reproducing CI Locally

The CI build is the same CMake plus Rux test flow documented in the [Development Workflow](Workflow.md). To reproduce the Linux required check:

```sh
cmake -S . -B Build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=clang++-22 \
  -DRUX_WERROR=ON \
  -DRUX_BUILD_TESTS=ON
cmake --build Build --config Release --parallel
ctest --test-dir Build --output-on-failure -C Release
./Bin/rux check
./Bin/rux lint
./Bin/rux test --release
```

Adjust the compiler executable for the host platform. Run the test command from the repository root so it finds the workspace manifest and the centralized `Tests/` tree.

To reproduce the Linux cross job, build and check the target without launching it:

```sh
./Bin/rux check --target linux-aarch64
./Bin/rux --manifest Tests/Language/Arithmetic/Rux.toml build --release --target linux-aarch64
```

On an AArch64 Linux machine, `sh Test.sh --target linux-aarch64` adds the policy checks, format pass, C++ unit tests, and directly executed Rux target tests. A physical x86-64 machine refuses that target test run before compiling the suite.

The Windows required check is the same flow, prefixed by the developer-environment step CI uses. From PowerShell at the repository root:

```powershell
./.github/scripts/Enter-VsDevEnv.ps1 -Arch amd64   # arm64 on an AArch64 host

cmake -S . -B Build -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_CXX_COMPILER=clang++ `
  -DCMAKE_CXX_COMPILER_TARGET=x86_64-pc-windows-msvc `
  -DRUX_WERROR=ON `
  -DRUX_BUILD_TESTS=ON
cmake --build Build --config Release --parallel
ctest --test-dir Build --output-on-failure -C Release
./Bin/rux.exe check
./Bin/rux.exe lint
./Bin/rux.exe test --release
```

On AArch64 Windows, the cross-target portion can be reproduced with either a native or emulated x86-64 `rux.exe`:

```powershell
./Bin/rux.exe check --target windows-aarch64
./Bin/rux.exe test --release --target windows-aarch64
./Tests/Native/WindowsAArch64ExitCode/Verify.ps1 -Rux ./Bin/rux.exe
./Tests/Native/WindowsAArch64Dll/Verify.ps1 -Rux ./Bin/rux.exe
```

The last two commands are repository fixtures rather than installed-compiler commands. A physical x86-64 Windows host can build and check this target, but Rux refuses to execute AArch64 target tests there; transfer the output to an AArch64 Windows machine for testing.

## Infrastructure Notes

- **Runner images** — Linux uses `ubuntu-24.04` and `ubuntu-24.04-arm`; Windows uses `windows-2025` and `windows-11-arm`; macOS uses `macos-26-intel` and `macos-26`. FreeBSD runs on an `ubuntu-24.04` host and boots x86-64/AArch64 guests in QEMU. GitHub's `windows-11-arm` runner is the default Windows AArch64 environment. An Azure Windows 11 ARM64 VM is reserved for interactive crash dumps, prolonged debugging, or demonstrated GitHub-runner instability; it is not an acceptance dependency. There are **no self-hosted runners** in the normal matrix.
- **Workflow security** — validation jobs have read-only repository permissions and checkouts do not persist credentials. Only the release publishing job receives `contents: write`.
- **Tool versions** — CMake and Ninja are pinned centrally in each workflow so runner-image changes do not silently change the build toolchain.
- **Architecture names** — prose and check labels use x86-64/AArch64; matrix values and artifact names use `x86_64`/`aarch64`. Runner, Visual Studio, and VM inputs retain the exact spellings required by those external tools.
- **Artifacts** — intermediate binaries are architecture-labelled with `x86_64` or `aarch64` and retained for seven days. Release archives include `SHA256SUMS` for integrity verification.
- **Caching** — none is configured today; each job starts with a fresh package cache and build directory. If build times become a problem, the natural next step is caching compiler downloads, the CMake/Ninja build directory, or the Rux package cache.
