# CI/CD Flow

Continuous integration runs on every push and pull request to `main` and `dev`,
building and testing Rux across every supported operating system. Continuous
delivery (publishing binaries) is covered separately in the
[Release Pipeline](Release.md).

## Per-OS build & test workflows

Each supported platform has its own workflow under
[`.github/workflows/`](../.github/workflows/):

| Workflow      | Platform            | Runner            | Toolchain install         |
|---------------|---------------------|-------------------|---------------------------|
| `linux.yml`   | Ubuntu 24.04        | GitHub-hosted     | `apt.llvm.org` → Clang 22 |
| `windows.yml` | Windows Server 2025 | GitHub-hosted     | Runner's bundled Clang    |
| `macos.yml`   | macOS 26            | GitHub-hosted     | Homebrew `llvm@22`        |
| `bsd.yml`     | FreeBSD 14.2        | QEMU VM on Ubuntu | `pkg llvm22`              |
| `illumos.yml` | OmniOS r151052      | QEMU VM on Ubuntu | `pkg install clang` (IPS) |

Their status is shown by the badges at the top of the [README](../README.md).

Two additions run alongside the per-OS matrix:

- **`lint.yml`** — repository-wide checks: the platform-isolation guard
  (`Tools/PlatformIsolation/Check.sh`, which fails when OS APIs like
  `getenv`/`<windows.h>`/`fork` are used outside `Compiler/System/`) and a
  `clang-format-22 --dry-run -Werror` pass over `Compiler/` and `Tests/Unit/`
  (excluding vendored `ThirdParty` code).
- **GCC job** (in `linux.yml`) — builds the compiler and runs the unit tests
  with `g++-14` to keep the codebase portable beyond Clang/MSVC. Warnings are
  not fatal there; the Clang jobs carry `-Werror`.

The Linux and Windows build jobs also configure with `-DRUX_BUILD_TESTS=ON` and
run the C++ unit tests (doctest via `ctest`) before uploading the binary
artifact; see [Development Workflow](Workflow.md) for the test layout.

## Triggers

```yaml
on:
  push:
    branches: [ main, dev ]
  pull_request:
    branches: [ main, dev ]
```

Superseded runs on the same ref are cancelled automatically
(`concurrency: cancel-in-progress`), so only the latest commit on a branch or PR
keeps running.

## What each run does

Every workflow is two jobs — **Build**, then **Test** (`needs: build`). Splitting
them means the binary is compiled once, uploaded as an artifact, then downloaded
by the test job. Using `linux.yml` as the reference shape:

1. **Build job**
    - Check out the repo.
    - Install Clang 22, plus CMake and Ninja.
    - Configure and build Release (Clang jobs add `-DRUX_WERROR=ON`, so
      warnings fail the build):
      ```sh
      cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++-22 -DRUX_WERROR=ON
      cmake --build build --config Release
      ```
    - Upload the `rux` binary as an artifact.
2. **Test job** (`needs: build`)
    - Download the built binary and restore its executable bit.
    - `./Bin/Release/rux install Std` (test packages depend on Std).
    - `./Bin/Release/rux test --release` from the repo root (workspace mode discovers `Tests/`).

### Platform-specific quirks

The native-runner workflows differ only in how the compiler is obtained; the
emulated ones differ in *where the whole job runs*:

- **Ubuntu** — installs Clang 22 from `apt.llvm.org` and builds with
  `clang++-22`.
- **Windows** — uses the runner's bundled Clang. Before configuring, it locates
  Visual Studio with `vswhere` and initializes the VS dev environment
  (`Launch-VsDevShell.ps1`) so Clang can find the Windows SDK, CRT headers, and
  import libraries.
- **macOS** — Apple Clang lags upstream and lacks full C++26 support, so the
  workflow installs LLVM `llvm@22` from Homebrew and points
  `CMAKE_CXX_COMPILER` at the Homebrew `clang++`.
- **FreeBSD (`bsd.yml`) and OmniOS (`illumos.yml`)** — GitHub has no native
  runner for these, so each job boots its own OS image in a QEMU VM (via the
  `vmactions/*-vm` actions) on an Ubuntu host. Because Build and Test are
  separate jobs, each boots a *fresh* VM and installs the toolchain again — and
  the Test VM additionally installs the Clang runtime libraries the prebuilt
  binary links against. The compiler binary name varies by packaging: versioned
  (`clang++22` on FreeBSD) where the OS ships versioned LLVM packages, plain
  `clang++` on OmniOS.

## Required checks

The following must pass before a PR can merge (configured in branch protection —
see [Branch Architecture](Branches.md)):

- **`linux.yml`** (Ubuntu 24.04)
- **`windows.yml`** (Windows Server 2025)

The remaining workflows — `macos.yml`, `bsd.yml` (FreeBSD), and `illumos.yml`
(OmniOS) — run on every push and PR and report status, but are
**informational**: they broaden platform coverage without blocking merges, since
the emulated VMs are slower and occasionally flaky. A red informational check is
still worth investigating before merging.

## Reproducing CI locally

The CI build is the same `cmake` + `rux test` flow documented in the
[Development Workflow](Workflow.md). To match CI exactly, build
`Release` and run `rux test --release`.

## Infrastructure notes

- **Runner images** — Ubuntu, Windows, and macOS use GitHub-hosted runners
  (`ubuntu-24.04`, `windows-2025`, `macos-26`). The BSD/illumos workflows run on
  `ubuntu-24.04` hosts and boot their target OS in a QEMU VM via the
  `vmactions/*-vm` actions. There are **no self-hosted runners**.
- **Caching** — none is configured today; each run installs its toolchain and
  builds from scratch. If build times become a problem, the natural next step is
  caching the CMake/Ninja build directory or the per-OS package installs.
