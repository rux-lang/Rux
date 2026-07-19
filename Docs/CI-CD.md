# CI/CD Flow

Continuous integration runs on every push and pull request to `main` and `dev`, building and testing Rux across every supported operating system. Continuous delivery (publishing binaries) is covered separately in the [Release Pipeline](Release.md).

## Per-OS build & test workflows

Each supported platform has its own workflow under [`.github/workflows/`](../.github/workflows/):

| Workflow      | Platform            | Runner            | Toolchain install         |
| ------------- | ------------------- | ----------------- | ------------------------- |
| `FreeBSD.yml` | FreeBSD 14.4 x86-64 | QEMU VM on Ubuntu | `pkg llvm22`              |
| `Linux.yml`   | Ubuntu 24.04        | GitHub-hosted     | `apt.llvm.org` → Clang 22 |
| `macOS.yml`   | macOS 26 x86-64     | GitHub-hosted     | Homebrew `llvm@22`        |
| `Windows.yml` | Windows Server 2025 | GitHub-hosted     | Runner's bundled Clang    |

Their status is shown by the badges at the top of the [README](../README.md).

Two repository-policy workflows run alongside the per-OS matrix:

- **`CodeQuality.yml`** — repository-wide checks: the platform-isolation guard (`Tests/Policy/PlatformIsolation/Check.sh`, which fails when OS APIs like `getenv`/`<windows.h>`/`fork` are used outside `Compiler/System/`), a `clang-format-22 --dry-run -Werror` pass, and parallel `clang-tidy-22` static analysis over the maintained C++ translation units in CMake's compilation database.
- **`PullRequestPolicy.yml`** — rejects pull requests targeting `main` and directs contributors to the `dev` integration branch.

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

## What each run does

Each per-OS validation workflow is two jobs — **Build**, then **Test** (`needs: build`). Splitting them means the binary is compiled once, uploaded as an artifact, then downloaded by the test job. Using `Linux.yml` as the reference shape:

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

### Platform-specific quirks

The native-runner workflows differ only in how the compiler is obtained; the emulated ones differ in _where the whole job runs_:

- **Ubuntu** — installs Clang 22 from `apt.llvm.org` and builds with `clang++-22`.
- **Windows** — uses the runner's bundled Clang. Before configuring, it locates Visual Studio with `vswhere` and initializes the VS dev environment (`Launch-VsDevShell.ps1`) so Clang can find the Windows SDK, CRT headers, and import libraries.
- **macOS** — Apple Clang lags upstream and lacks full C++26 support, so the workflow installs LLVM `llvm@22` from Homebrew and points `CMAKE_CXX_COMPILER` at the Homebrew `clang++`.
- **FreeBSD** — GitHub has no native FreeBSD runner, so each job boots FreeBSD 14.4 in a QEMU VM via `vmactions/freebsd-vm` on an Ubuntu host. Because Build and Test are separate jobs, each boots a _fresh_ VM; the Test VM installs the Clang runtime libraries needed by the prebuilt binary.

## Required checks

The following must pass before a PR can merge (configured in branch protection — see [Branch Architecture](Branches.md)):

- **`Linux.yml`** (Ubuntu 24.04)
- **`Windows.yml`** (Windows Server 2025)

The remaining workflows — `macOS.yml` and `FreeBSD.yml` — run on every push and PR and report status, but are **informational**: they broaden platform coverage without blocking merges, since non-required platforms can be slower or occasionally flaky. A red informational check is still worth investigating before merging.

## Reproducing CI locally

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

## Infrastructure notes

- **Runner images** — Ubuntu, Windows, and macOS use GitHub-hosted runners (`ubuntu-24.04`, `windows-2025`, `macos-26-intel`). FreeBSD runs on an `ubuntu-24.04` host and boots its target OS in a QEMU VM. There are **no self-hosted runners**.
- **Workflow security** — validation jobs have read-only repository permissions and checkouts do not persist credentials. Only the release publishing job receives `contents: write`.
- **Tool versions** — CMake and Ninja are pinned centrally in each workflow so runner-image changes do not silently change the build toolchain.
- **Artifacts** — intermediate binaries are architecture-labelled and retained for seven days. Release archives include `SHA256SUMS` for integrity verification.
- **Caching** — none is configured today; each job starts with a fresh package cache and build directory. If build times become a problem, the natural next step is caching compiler downloads, the CMake/Ninja build directory, or the Rux package cache.
