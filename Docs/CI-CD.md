# CI/CD Flow

Continuous integration runs on every push and pull request to `main` and `dev`, building and testing Rux across every supported operating system. Continuous delivery (publishing binaries) is covered separately in the [Release Pipeline](Release.md).

The workflow matrices described here build the Rux compiler on native runners. They are distinct from the package compiler's [`rux build --all` matrix](Builds.md#building-the-complete-matrix), which one Rux compiler process uses to produce 16 target/profile artifact cells.

## Per-OS Build and Test Workflows

Each supported platform has its own workflow under [`.github/workflows/`](../.github/workflows/):

| Workflow      | Platform                                                                             | Runner            | Toolchain install           |
| ------------- | ------------------------------------------------------------------------------------ | ----------------- | --------------------------- |
| `FreeBSD.yml` | FreeBSD 15.1 x86-64 and AArch64                                                      | QEMU VM on Ubuntu | `pkg llvm23`                |
| `Linux.yml`   | Ubuntu 26.04 x86-64 and AArch64, plus Linux/macOS AArch64 cross builds               | GitHub-hosted     | `apt.llvm.org` → Clang 23   |
| `macOS.yml`   | macOS 26 Intel and Apple Silicon, plus an x86-64 compiler → AArch64 target cross job | GitHub-hosted     | Homebrew `llvm@23`          |
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
   - The final `FreeBSD acceptance` job uses `if: always()` and checks both native architectures and the transferred-runtime dependency explicitly. A failed or skipped native matrix or transferred runtime therefore produces one stable failing check for branch protection.

No cross job installs a target compiler, assembler, linker, archiver, or signer: Rux encodes, links, archives, and signs the target formats itself. Linux and Windows treat Mach-O as build-only foreign output. The Apple Silicon macOS job can execute `macos-aarch64` output directly, including when the compiler process runs under Rosetta. Native Windows, macOS, and FreeBSD AArch64 jobs run their complete language/package suites and platform fixtures. FreeBSD additionally requires transferred x86-64-compiler acceptance. The release workflow repeats these native acceptance sets and makes FreeBSD transferred acceptance a direct dependency of publication.

### Platform-Specific Quirks

The native-runner workflows differ only in how the compiler is obtained; the emulated ones differ in _where the whole job runs_:

- **Ubuntu** — restores cached or prepared Clang 23, bootstrapping from `apt.llvm.org` only on a cache miss and builds with `clang++-23` on `ubuntu-26.04` (x86-64) and `ubuntu-26.04-arm` (AArch64). Clang is the host C++ compiler that builds `rux`, and nothing else: the AArch64 test and cross jobs run the compiler's own back end.
- **Windows** — neither image clears the Clang 23.1 floor (`windows-2025` preinstalls Clang 20, and `windows-11-arm` only has the Clang 19 bundled with Visual Studio), so the workflow restores the upstream llvm.org release for the host architecture: the matching Windows MSVC archive on x86-64 and ARM64, verified against the pinned SHA-256 in `.github/CI/Install/WindowsLLVM.ps1` and unpacked to `C:\LLVM`. Before native builds, `.github/Scripts/Install.ps1 -Tool VsEnvironment` locates Visual Studio with `vswhere` and imports the matching x86-64 or ARM64 toolset for the Windows SDK and CRT; because that also puts the Visual Studio Clang on `PATH`, `CMAKE_CXX_COMPILER` is given the absolute path to the installed one, along with the explicit MSVC target triple. The cross job needs neither setup step: it downloads the already-built x86-64 compiler and relies on Windows-on-ARM only to run that compiler.
- **macOS** — Apple Clang lags upstream and lacks full C++26 support, so the workflow restores LLVM `llvm@23` prepared with Homebrew and points `CMAKE_CXX_COMPILER` at the Homebrew `clang++`. `llvm@23` is only an alias of the current `llvm` formula until LLVM 24 gives it a versioned formula of its own, and a runner image's formula snapshot can predate the LLVM 23 bump, so the bootstrap step runs `brew update` first and then verifies that the installed `clang++` reports major version 23. The `macos-26` Apple Silicon image is the deployment baseline and native acceptance environment; the cross job uses its built-in Rosetta support only to run the x86-64 compiler, never to run the generated ARM64 programs.
- **FreeBSD** uses separate prepared FreeBSD 15.1 QEMU images for build, runtime, and transferred-artifact acceptance. GitHub has no native FreeBSD runner. The x86-64 guest requires KVM on Ubuntu 26.04; AArch64 remains emulated on Ubuntu 24.04 for compatible firmware. The prepared runtime image supplies Rux library dependencies, and the minimal transferred-artifact image contains no added compiler tools.

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
./.github/Scripts/Install.ps1 -Tool VsEnvironment -Arch amd64   # arm64 on an AArch64 host

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

- **Runner images** — Linux uses `ubuntu-26.04` and `ubuntu-26.04-arm`; Windows uses `windows-2025` and `windows-11-arm`; macOS uses `macos-26-intel` and `macos-26`. FreeBSD boots its x86-64 guests in QEMU on an `ubuntu-26.04` host and its emulated AArch64 guests on `ubuntu-24.04`, the only host image the VM action validates for them. GitHub's `windows-11-arm` and `macos-26` runners are the normal AArch64 acceptance environments. Azure Windows 11 ARM64 and EC2 Mac are escalation-only options for interactive crash dumps, prolonged debugging, or demonstrated GitHub-runner instability; neither is an acceptance dependency. There are **no self-hosted runners** in the normal matrix.
- **Workflow security** — validation jobs have read-only repository permissions and checkouts do not persist credentials. Product release publishing and manually dispatched environment preparation receive `contents: write`; ordinary validation remains read-only.
- **Tool versions** — CMake and Ninja are pinned centrally in each workflow so runner-image changes do not silently change the build toolchain.
- **Architecture names** — prose and check labels use x86-64/AArch64; matrix values and artifact names use `x86_64`/`aarch64`. Runner, Visual Studio, and VM inputs retain the exact spellings required by those external tools.
- **Artifacts** — intermediate binaries are architecture-labelled with `x86_64` or `aarch64` and retained for seven days. Release archives include `SHA256SUMS` for integrity verification. The macOS build jobs retry a failed binary upload once after a short pause: hosted macOS runners intermittently fail `CreateArtifact` with transient DNS errors (`ENOTFOUND`), and `actions/upload-artifact` has no built-in retry.
- **Caching** — none is configured today; each job starts with a fresh package cache and build directory. If build times become a problem, the natural next step is caching compiler downloads, the CMake/Ninja build directory, or the Rux package cache.

## Build Caches and Test Workers

Native validation builds use `CMAKE_CXX_COMPILER_LAUNCHER=ccache`. Cache keys separate host OS (including FreeBSD VM release), architecture, Clang 23, runtime, and Release configuration; compiler-content checks prevent reuse across differing compiler builds. PCH is explicitly disabled in cache jobs and in the clang-tidy compilation database. Windows validation and release builds both use the pinned upstream archive from `WindowsLLVM.ps1`; release builds do not rely on the runner's older Clang.

In CI, CTest and `rux test --jobs N` use up to four available processors, bounded by the CPU count reported by the runner or FreeBSD VM: the POSIX jobs share `Scripts/TestJobs.sh` and the PowerShell jobs use the same bound. The local entry points default to one worker per logical processor instead. Doctest source groups are disjoint and verified, fixture groups are resource-locked, and repository/installer checks run as ordinary CTest tests. Code Quality owns the architectural policy job. See the workflow guide's [build and test throughput](Workflow.md#build-and-test-throughput) section for local caches, PCH, ThinLTO, and how to measure a change.

### Build-tool versions

Package-based LLVM bootstrap accepts upstream 23.1+ within major 23, including package patch updates such as 23.1.1. Windows retains its checksum-pinned 23.1.0 archive. Published bundles remain fixed by their asset hashes.

Native runners restore CMake 4.4.3 and Ninja 1.13.2 through `.github/Scripts/Install.ps1 -Tool BuildTools`. On a cache miss, the installer uses checksum-verified upstream binary archives. LLVM installations are restored from a tool cache or published native bundle; only bootstrap cache misses use apt/Homebrew or the Windows archive. Git comes from the hosted runner. FreeBSD consumer jobs require a published image containing the pinned tools; they never compile CMake or install guest packages. Before image promotion they fail with a preparation instruction. The source CMake installer is restricted to explicit environment preparation and local developer bootstrap.

### Prepared environments and activation

Keep preparation in this repository: tool recipes, validation, and consumers must evolve together. A CMake fork is unnecessary. `PrepareTools.yml` with **FreeBSD** selected builds five FreeBSD 15.1 images: build and runtime images for x86-64/AArch64, and a minimal AArch64 image for transferred-artifact acceptance. Runtime images contain the actual shared-library closure of Rux. The minimal image does not install LLVM, CMake, Ninja, or Git. Every role is tested on a fresh boot. The upstream base image and its public bootstrap SSH key are checksum-pinned. SSH is bound to host loopback; these disposable CI images are not general-purpose server images.

The same workflow with **Native** selected builds six tool bundles for the standard Linux, Windows, and macOS runners on both architectures. A separate clean job restores each bundle and builds/tests Rux before publication. Linux and macOS use `Run.sh`; Windows uses `Run.ps1` after importing its Visual Studio environment. Homebrew bundles include the union of LLVM and ccache dependencies. If validation exposes a packaging defect, fix the recipe and dispatch a new revision; rerunning validation alone keeps the old bundle bytes. Leave the failed draft unpromoted. Full custom hosted Windows/macOS images are not necessary: bundles preserve the required tools while retaining GitHub's standard SDK/runtime images. Linux also uses a bundle; container/image changes should only replace it after matched measurements show a benefit. Host QEMU support may still require installation on Linux; prepared FreeBSD guests need no package setup.

Run **Prepare Tools** on trusted `dev`, selecting **All**, **FreeBSD**, or **Native**. Enter only a revision suffix such as `2026-09-06-v2`; the workflow creates separate `ci-tools-freebsd-2026-09-06-v2` and/or `ci-tools-native-2026-09-06-v2` releases. The two groups run and publish independently. `Actions/PrepareFreeBSD/action.yml` contains the shared FreeBSD phase steps. It is a composite action, not another workflow; only `PrepareTools.yml` is run manually. It writes a draft prerelease, then publishes only after all validation succeeds. These tags do not trigger the `v*` product release workflow, and environment releases never become the latest product release. Nothing is published by a push or PR. FreeBSD CMake work is checkpointed across four two-hour build phases, so emulated AArch64 compilation can resume without losing completed objects. To resume a FreeBSD draft checkpoint, select **FreeBSD**, enable **resume**, and enter its original revision suffix (for an existing `ci-tools-freebsd-v1` draft, enter `v1`). Resume is rejected with All or Native selected. Use the same recipe for checkpoints; use a new revision when changing the recipe. The first preparation is intentionally expensive, once per tool update.

The checked-in `.github/CI/Environments.lock.json` starts empty: no untested image is silently selected. Manual dispatch requires `PrepareTools.yml` to exist on the default branch (`main`), even when selecting `dev` as the execution branch. Register those workflow definitions on `main` through the repository's normal reviewed promotion process before attempting the first dispatch. A `dev`-only workflow does not provide the Actions UI's Run workflow button.

Activation requires the following steps after that registration:

1. Dispatch **Prepare Tools** on `dev` with **All** selected, a new revision suffix, and **resume** unchecked. You can select FreeBSD or Native to prepare just one group.
2. Wait for the selected groups to finish validation and publication.
3. Review the clean validation jobs, download each published `candidate.json` into a different directory under `BuildCache/`, and promote each with `sh .github/Scripts/Run.sh environment promote --candidate PATH` or `./.github/Scripts/Run.ps1 -Task Environment promote --candidate PATH`. Promotion verifies every asset checksum before editing the lock; allow disk space for these downloads. Review the resulting lock diff.
4. Include the promoted lock in the repository change and run all ordinary platform and release checks. Keep the preceding environment release for rollback; reverting its lock entries selects the previous tools immediately.

Standard hosted public-repository runners are used throughout, with no larger runners, paid image service, or self-hosted infrastructure. Keep the account's paid cache spending disabled and retain the default cache quota: eviction falls back to published tool assets. Compiler cache entries are bounded to 768 MiB per architecture; tool caches share the repository quota and may be evicted. Do not assume all caches stay warm.

### Timing and worker selection

The supplied baseline at commit `666152d` is Linux 8m43s, Windows 15m55s, macOS 23m08s, Code Quality 13m06s, and FreeBSD 4h00m14s (timeout). The failed FreeBSD AArch64 job never reached the Rux build: most time was spent configuring and compiling CMake. Prepared images remove that work from ordinary CI. Native compiler caches are saved before later test steps, and each architecture's test jobs depend only on its own build. Clang-tidy is split into three disjoint shards with an aggregate required check; source coverage is unchanged.

`./.github/Scripts/Run.ps1 -Task Benchmark` or `sh .github/Scripts/Run.sh benchmark` is an optional developer measurement command against an existing build. It compares one, two, and four workers (bounded by CPU count), alternates sample order, and retains individual CTest and Rux timings under `BuildCache/worker-benchmark.json`. It is never run by push/PR jobs and does not select an optimum automatically. Keep up to four workers until repeated measurements on the actual CI hosts justify a different value; free runners do not imply serial is faster. Record cold and warm workflow durations separately after activation. No new elapsed-time claim is established by local syntax checks or by the preparation design alone.

### Script layout

Repository-owned directories and helper files under `.github` use PascalCase: `Actions/BuildTools`, `CI/Install`, `Scripts/Install.ps1`, `Verify.ps1`, and `Run.ps1`, with POSIX counterparts. Public scripts dispatch by purpose; private installers and image management live under `CI`. Preserve GitHub's special names (`workflows`, `action.yml`, `ISSUE_TEMPLATE`, `config.yml`, and community metadata). `macOS.yml` retains the platform spelling and its existing workflow URL. Command arguments and workflow job identifiers retain their established spelling.
