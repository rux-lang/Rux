# CI/CD Flow

Continuous integration runs on every push and pull request to `main` and `dev`, building and testing Rux across every supported operating system. Continuous delivery (publishing binaries) is covered separately in the [Release Pipeline](Release.md).

The workflow matrices described here build the Rux compiler on native runners. They are distinct from the package compiler's [`rux build --all` matrix](Builds.md#building-the-complete-matrix), which one Rux compiler process uses to produce 16 target/profile artifact cells.

## Per-OS Build and Test Workflows

Each supported platform has its own workflow under [`.github/workflows/`](../.github/workflows/):

| Workflow      | Platform                                                                             | Runner            | Toolchain install             |
| ------------- | ------------------------------------------------------------------------------------ | ----------------- | ----------------------------- |
| `FreeBSD.yml` | FreeBSD 15.1 x86-64 and AArch64                                                      | QEMU VM on Ubuntu | `pkg llvm23`                  |
| `Linux.yml`   | Ubuntu 26.04 x86-64 and AArch64, plus Linux/macOS AArch64 cross builds               | GitHub-hosted     | `apt.llvm.org` → Clang 23     |
| `macOS.yml`   | macOS 26 Intel and Apple Silicon, plus an x86-64 compiler → AArch64 target cross job | GitHub-hosted     | Homebrew `llvm@23`            |
| `Windows.yml` | Windows 2025 and Windows 11 ARM, plus Windows/macOS AArch64 cross coverage           | GitHub-hosted     | llvm.org archive → Clang 23 |

Their status is shown by the badges at the top of the [README](../README.md).

Two repository-policy workflows run alongside the per-OS matrix:

- **`CodeQuality.yml`** — one architectural-boundary job runs host API isolation, internal code generation/linking, and CLI process-output ownership checks. Separate jobs check formatting and static analysis. Language behavior and message rendering are tested through the compiler and CLI; file length is reviewed without an automated gate.
- **`BranchPolicy.yml`** — rejects pull requests targeting `main` and directs contributors to the `dev` integration branch.

### The External-Toolchain Guard

`Tests/Policy/NoExternalToolchain/Check.sh` protects the property the whole compiler is built around: Rux encodes its own machine code, writes its own object files, links its own executables, and signs Mach-O images in-process, so no part of it may shell out to a build tool. The check greps `Compiler/` for two things and fails on either —

- a string literal naming an assembler, C compiler, linker, archiver, or signing tool, under any spelling a real one carries: a path (`/usr/bin/clang`), a cross prefix (`aarch64-linux-gnu-gcc`), a version suffix (`clang-23`) or a Windows extension (`link.exe`). Two-letter names like `as`, `cc` and `ld` are assembler mnemonics and register names throughout `CodeGen/`, so they count only when the literal also carries a directory or an extension;
- a call to `System::RunInherited` or `System::RunCaptured` — the two entry points every process launch goes through — from outside `Compiler/System/`.

No file is allowed to name a toolchain program. The second check has a short allowlist at the top of the script, and a file joins it only with a reason written beside it: running a program is not the same as building one, so `Cli/CmdRun.cpp` and `Cli/Testing/TestExecution.cpp` are its two permanent entries, directly executing the host artifact or a directly executable same-OS target test.

The guard runs in the shared architectural-boundary job in `CodeQuality.yml` and as the first step of `sh Run.sh test` and `./Run.ps1 test`, beside the platform-isolation check. The shared job covers the FreeBSD sources as well; VM builds do not repeat these source scans.


### Minimal Source-Tree Policy

`sh Run.sh policy` runs only the three architectural boundaries. `Tests/Policy/OutputOwnership/Exceptions.txt` records the stable inspection and generated-output formats allowed outside CLI. Repository-command and installer checks are ordinary CTest tests in `Tests/Scripts/`; they run alongside the unit tests rather than in policy. Line-count limits, language-cutover scanners and message-style scanners have been removed.

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

Each per-OS validation workflow has an x86-64/AArch64 matrix with two stages — **Build**, then **Test** (`needs: build`). Splitting them means each native binary is compiled once, uploaded as an architecture-labelled artifact, then downloaded by the matching test job. Using `Linux.yml` as the reference shape:

1. **Build job**
   - Check out the repo.
   - Install Clang 23, plus pinned CMake and Ninja versions.
   - Configure and build Release (Clang jobs add `-DRUX_WERROR=ON`, so warnings fail the build):
     ```sh
     cmake -S . -B Build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++-23 -DRUX_WERROR=ON -DRUX_BUILD_TESTS=ON
     cmake --build Build --config Release --parallel
     ctest --test-dir Build --output-on-failure -C Release
     ```
   - Upload the `rux` binary as an artifact.
2. **Test job** (`needs: build`)
   - Download the built binary and restore its executable bit.
   - On Linux, verify Rux formatting across every package and test manifest.
   - Run `rux check`, `rux lint`, and `rux test --release` from the repo root. Workspace mode discovers every language and package test below `Tests/`, resolves first-party dependencies locally, and disables registry fallback.

The platform workflows also add cross-target coverage to those two native
stages.

3. **Linux cross job** (`needs: build`, `ubuntu-26.04`)
   - Download the x86-64 binary built by the build job.
   - Run `rux check --target linux-aarch64`, build a representative AArch64 executable, and inspect its repository-produced ELF header for `EM_AARCH64`. The x86-64 host never launches the output. `rux lint` takes no target and is not repeated here.
   - Check `macos-aarch64`, build a signed executable and dylib twice, and use the repository's portable verifier to check deterministic bytes, the ARM64 Mach-O headers, load-command ranges, CodeDirectory fields, and every SHA-256 code-slot hash. Linux never launches either Mach-O image.

4. **Windows cross job** (`needs: build`, `windows-11-arm`)
   - Download the x86-64 Windows compiler and run it through Windows-on-ARM emulation. No compiler toolchain is installed in this job.
   - Run `rux check --target windows-aarch64` and `rux test --release --target windows-aarch64`. The emulated compiler detects that the underlying OS is AArch64, so the programs it produces execute natively.
   - Build and run the dedicated executable exit-code and DLL load/call/unload fixtures. These protect process launch, PE entry, imports, exports, and import-library handling beyond the workspace suite.
   - Cross-build the same signed `macos-aarch64` executable and dylib smoke fixtures twice and inspect their deterministic bytes and signatures without launching them.

5. **macOS cross job** (`needs: build`, `macos-26`)
   - Download the thin x86-64 macOS compiler and run it under Rosetta on the underlying Apple Silicon host.
   - Run `rux check --target macos-aarch64` and the complete direct-only `rux test --release --target macos-aarch64` suite. The compiler process is translated, but the produced ARM64 programs launch directly on the native OS.
   - Run the Apple Silicon exit-code, fixed/variadic libSystem, assertion/panic, and dylib load/call/unload fixtures with the x86-64 compiler. Each fixture checks its ARM64 header and in-process ad-hoc signature before native execution.

6. **FreeBSD native and transferred-cross acceptance**
   - Both architecture build jobs run the source-tree policies, C++ and Rux format checks, and C++ unit tests. Both test jobs run workspace check, lint, and the complete `rux test --release` suite.
   - The AArch64 test job additionally runs the freestanding, libc fixed/variadic, assertion/panic, BSD syscall, and shared-library fixtures directly.
   - A separate x86-64 VM downloads the x86-64 compiler and creates a target-only `freebsd-aarch64` payload. A fresh AArch64 VM installs no compiler, verifies the payload manifest, hashes, modes, and ELF identity, then launches it directly.
   - The final `FreeBSD acceptance` job uses `if: always()` and checks both dependency results explicitly. A failed or skipped native matrix or transferred runtime therefore produces one stable failing check for branch protection.

No cross job installs a target compiler, assembler, linker, archiver, or signer: Rux encodes, links, archives, and signs the target formats itself. Linux and Windows treat Mach-O as build-only foreign output. The Apple Silicon macOS job can execute `macos-aarch64` output directly, including when the compiler process runs under Rosetta. Native Windows, macOS, and FreeBSD AArch64 jobs run their complete language/package suites and platform fixtures. FreeBSD additionally requires transferred x86-64-compiler acceptance. The release workflow repeats these native acceptance sets and makes FreeBSD transferred acceptance a direct dependency of publication.

### Platform-Specific Quirks

The native-runner workflows differ only in how the compiler is obtained; the emulated ones differ in _where the whole job runs_:

- **Ubuntu** — installs Clang 23 from `apt.llvm.org` and builds with `clang++-23` on `ubuntu-26.04` (x86-64) and `ubuntu-26.04-arm` (AArch64). Clang is the host C++ compiler that builds `rux`, and nothing else: the AArch64 test and cross jobs run the compiler's own back end.
- **Windows** — neither image clears the Clang 23.1 floor (`windows-2025` preinstalls Clang 20, and `windows-11-arm` only has the Clang 19 bundled with Visual Studio), so the workflow installs the upstream llvm.org release for the host architecture: the matching Windows MSVC archive on x86-64 and ARM64, verified against the pinned SHA-256 in `.github/scripts/Install-Llvm.ps1` and unpacked to `C:\LLVM`. Before native builds, `.github/scripts/Enter-VsDevEnv.ps1` locates Visual Studio with `vswhere` and imports the matching x86-64 or ARM64 toolset for the Windows SDK and CRT; because that also puts the Visual Studio Clang on `PATH`, `CMAKE_CXX_COMPILER` is given the absolute path to the installed one, along with the explicit MSVC target triple. The cross job needs neither setup step: it downloads the already-built x86-64 compiler and relies on Windows-on-ARM only to run that compiler.
- **macOS** — Apple Clang lags upstream and lacks full C++26 support, so the workflow installs LLVM `llvm@23` from Homebrew and points `CMAKE_CXX_COMPILER` at the Homebrew `clang++`. `llvm@23` is only an alias of the current `llvm` formula until LLVM 24 gives it a versioned formula of its own, and a runner image's formula snapshot can predate the LLVM 23 bump, so the install step runs `brew update` first and then verifies that the installed `clang++` reports major version 23. The `macos-26` Apple Silicon image is the deployment baseline and native acceptance environment; the cross job uses its built-in Rosetta support only to run the x86-64 compiler, never to run the generated ARM64 programs.
- **FreeBSD** — GitHub has no native FreeBSD runner, so each job boots an x86-64 or AArch64 FreeBSD 15.1 QEMU VM via `vmactions/freebsd-vm` on an Ubuntu host. Because Build and Test are separate jobs, each boots a _fresh_ VM; the Test VM installs the Clang runtime libraries needed by the prebuilt binary. Transferred acceptance uses distinct x86-64 producer and AArch64 consumer VMs, and only the producer installs a compiler runtime. The x86-64 VM is KVM-accelerated, but the AArch64 one is fully emulated and roughly an order of magnitude slower, so every VM is given the host's four cores and the AArch64 job timeouts are sized for emulation rather than native speed.

## Required Checks

The following must pass before a PR can merge (configured in branch protection — see [Branch Architecture](Branches.md)):

- **`CodeQuality.yml`** — one architectural-boundary job runs host API isolation, internal code generation/linking, and CLI process-output ownership checks. Separate jobs check formatting and static analysis. Language behavior and message rendering are tested through the compiler and CLI; file length is reviewed without an automated gate.
- **`FreeBSD acceptance`** from `FreeBSD.yml` (FreeBSD 15.1 x86-64/AArch64, native AArch64 fixtures, and transferred x86-64-to-AArch64 runtime acceptance)
- **`Linux.yml`** (Ubuntu 26.04 x86-64 and AArch64, and the AArch64 cross job)
- **`macOS.yml`** (macOS 26 Intel and Apple Silicon, full native ARM64 fixtures, and the Rosetta compiler cross job)
- **`Windows.yml`** (Windows x86-64 and AArch64, and the x86-64 compiler → AArch64 target cross job)

Branch protection requires the aggregate `FreeBSD acceptance` job. It depends on both the ordinary FreeBSD test matrix and transferred AArch64 runtime job, and fails explicitly if either path fails or is skipped.

## Reproducing CI Locally

The CI build is the same CMake plus Rux test flow documented in the [Development Workflow](Workflow.md). To reproduce the Linux required check:

```sh
cmake -S . -B Build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=clang++-23 \
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

On an AArch64 Linux machine, `sh Run.sh test --target linux-aarch64` adds the policy checks, format pass, C++ unit tests, and directly executed Rux target tests. A physical x86-64 machine refuses that target test run before compiling the suite.

On native FreeBSD AArch64, reproduce the ordinary and focused runtime paths with `sh Run.sh test` followed by `sh Tests/Native/FreeBSDAArch64/Verify.sh ./Bin/rux`. The transferred producer and consumer commands are documented in the [FreeBSD platform guide](Platforms/FreeBSD.md#cross-compiling); the consumer must run on a separate native AArch64 FreeBSD machine.

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
./Tests/Native/WindowsAArch64Assert/Verify.ps1 -Rux ./Bin/rux.exe
./Tests/Native/WindowsAArch64Panic/Verify.ps1 -Rux ./Bin/rux.exe
./Tests/Native/WindowsAArch64Dll/Verify.ps1 -Rux ./Bin/rux.exe
```

The last two commands are repository fixtures rather than installed-compiler commands. A physical x86-64 Windows host can build and check this target, but Rux refuses to execute AArch64 target tests there; transfer the output to an AArch64 Windows machine for testing.

On Apple Silicon, reproduce both native and x86-64-compiler acceptance paths:

```sh
./Bin/rux check --target macos-aarch64
./Bin/rux test --release --target macos-aarch64
sh Tests/Native/MacOSAArch64/Verify.sh ./Bin/rux
sh Tests/Native/MacOSAArch64/VerifyRosetta.sh /path/to/x86_64/rux
```

The Rosetta script requires a thin x86-64 compiler, but every emitted ARM64 image is launched directly. On Linux or Windows, run `Tests/Native/MacOSAArch64/VerifyCross.ps1` instead; it builds and inspects the signed images but deliberately never executes them.

## Infrastructure Notes

- **Runner images** — Linux uses `ubuntu-26.04` and `ubuntu-26.04-arm`; Windows uses `windows-2025` and `windows-11-arm`; macOS uses `macos-26-intel` and `macos-26`. FreeBSD runs on an `ubuntu-26.04` host and boots x86-64/AArch64 guests in QEMU. GitHub's `windows-11-arm` and `macos-26` runners are the normal AArch64 acceptance environments. Azure Windows 11 ARM64 and EC2 Mac are escalation-only options for interactive crash dumps, prolonged debugging, or demonstrated GitHub-runner instability; neither is an acceptance dependency. There are **no self-hosted runners** in the normal matrix.
- **Workflow security** — validation jobs have read-only repository permissions and checkouts do not persist credentials. Only the release publishing job receives `contents: write`.
- **Tool versions** — CMake and Ninja are pinned centrally in each workflow so runner-image changes do not silently change the build toolchain.
- **Architecture names** — prose and check labels use x86-64/AArch64; matrix values and artifact names use `x86_64`/`aarch64`. Runner, Visual Studio, and VM inputs retain the exact spellings required by those external tools.
- **Artifacts** — intermediate binaries are architecture-labelled with `x86_64` or `aarch64` and retained for seven days. Release archives include `SHA256SUMS` for integrity verification. The macOS build jobs retry a failed binary upload once after a short pause: hosted macOS runners intermittently fail `CreateArtifact` with transient DNS errors (`ENOTFOUND`), and `actions/upload-artifact` has no built-in retry.
- **Caching** — none is configured today; each job starts with a fresh package cache and build directory. If build times become a problem, the natural next step is caching compiler downloads, the CMake/Ninja build directory, or the Rux package cache.

## Build Caches and Test Workers

Native validation builds use `CMAKE_CXX_COMPILER_LAUNCHER=ccache`. Cache keys separate host OS (including FreeBSD VM release), architecture, Clang 23, runtime, and Release configuration; compiler-content checks prevent reuse across differing compiler builds. PCH is explicitly disabled in cache jobs and in the clang-tidy compilation database. Windows validation and release builds both use the pinned upstream archive from `Install-Llvm.ps1`; release builds do not rely on the runner's older Clang.

CTest and `rux test --jobs N` use up to four available processors. The POSIX jobs share `Scripts/TestJobs.sh`; PowerShell uses the same bound. Doctest source groups are disjoint and verified, fixture groups are resource-locked, and repository/installer checks run as ordinary CTest tests. Code Quality owns the architectural policy job. See [Compiler Build Performance](CompilerPerformance.md) for local cache, PCH, ThinLTO, and measurement commands.

### Build-tool versions

Native runners install checksum-verified CMake 4.4.3 and Ninja 1.13.2 archives using
`.github/scripts/Install-BuildTools.ps1`. FreeBSD uses the packaged Ninja (1.13.2+) and builds CMake 4.4.3 from
checksum-verified upstream sources with `.github/scripts/Install-CMake-FreeBSD.sh`; the private installation is
cached by FreeBSD release and architecture. Configuration requires CMake 4.4.3+, Ninja 1.13.2+, and upstream Clang
23.1+. All Ubuntu workflow hosts and native Linux runners use `ubuntu-26.04` or `ubuntu-26.04-arm`.
