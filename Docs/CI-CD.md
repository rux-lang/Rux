# CI/CD

Continuous integration builds and verifies Rux on all eight supported targets. No CI job compiles Clang, CMake, Ninja, or Git, and no job installs a target toolchain: Rux emits and links every target format in-process. What a run does depends on what triggered it, so an ordinary push gets an answer in minutes and anything that can reach `dev` gets the full matrix.

## Workflows

Two workflows, plus the community metadata GitHub owns.

- **`CI.yml`** — the one workflow that verifies Rux; every push, pull request, and manual run starts with it. Its first job resolves the verification scope, every target is one job that lists its steps in the order they run, the host-independent checks (policy guards, C++ formatting, clang-tidy, and the branch policy that rejects pull requests targeting `main`) are jobs beside them, and the last job is the single required check, **`CI`**. The run graph is flat: `Scope`, then the jobs, then `CI`.
- **`Release.yml`** — manual only (`workflow_dispatch`, with a version input and a dry-run switch). It calls `CI.yml` itself with the `extended` scope, so a release is built by the very jobs CI ran and nothing that runs under emulation is skipped, then packages what those jobs uploaded. Pushing a tag never starts a release by itself.

A step that several jobs share is written once in `CI.yml`, in the job that uses it first, with a YAML anchor (`- &build`), and repeated in the other jobs by alias (`- *build`). The shared steps — checkout, toolchain, compilation cache restore and save, build, test, runtime closure, upload — read the target from the job's `RUX_TARGET`, so an alias is the same step for another target and cannot drift from it, while a job page still shows every step with its own timing. Steps only one target runs sit inline in that target's job. No step is conditioned on a platform or a matrix, so a job page shows no skipped steps; the only conditions are the scope gates and the cache save, which runs on pushes.

The check `Tests/Scripts/CI/Check.sh` runs on every host through CTest as `Scripts.CI` and keeps the pieces from drifting: `CI.yml` and `Release.yml` are the only workflows, every target has a job that names it and uploads the compiler under the name `Release.yml` packages, every job has a timeout and is needed by the `CI` gate, and `Release.yml` builds through exactly one call to `CI.yml` in the extended scope.

## Scope

`CI.yml`'s first job runs `.github/Scripts/Scope.sh`, which decides the scope from the event and, for a push or a pull request, from the files that changed.

| Scope      | When                                                       | What runs                                                                                                                                                                       |
| ---------- | ---------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `docs`     | only documentation or community metadata changed           | nothing builds; the `CI` check reports success                                                                                                                                  |
| `fast`     | a push to a topic branch                                   | policy and formatting, and Linux x86-64: build, C++ and Rux suites, runtime closure, Rux formatting, every cross target checked, the macOS and FreeBSD cross-build inspections  |
| `full`     | a pull request to `dev` or `main`                          | `fast`, plus clang-tidy, Linux AArch64, macOS AArch64, macOS x86-64 under Rosetta, Windows x86-64, Windows AArch64, FreeBSD x86-64, and FreeBSD AArch64 with its emulated smoke |
| `extended` | a push to `dev` or `main`, a dispatched run, and a release | `full`, plus everything that runs under emulation: Windows x86-64 on an ARM64 host, the FreeBSD transfer test, and the complete Rux suites on FreeBSD AArch64                   |

A documentation-only change is one whose every file is Markdown under `Docs/` or at the repository root, or lives under `.github/` as Markdown, an issue template, or the funding file. Markdown anywhere else — a package license, the test-suite guide — is code, because something may read it. When the changed files cannot be listed, the scope never shrinks.

`workflow_dispatch` takes the scope as an input, `extended` by default, so any branch can be verified in full by hand. There is no scheduled run: pushes to `dev` carry the extended scope, and the caches stay warm as long as `dev` sees a push at least weekly.

The `main` branch is GitHub's default branch. Actions caches are visible to the branch that created them, to the base branch of a pull request, and to the default branch, so a pull request restores what `dev` cached and a topic branch's first push restores what `dev` or `main` cached.

## Jobs and timeouts

Every job carries an explicit timeout. Nothing can run for hours except the emulated FreeBSD suites, which are scoped so that nothing waits on them.

| Job                             | Runner                       | Waits for       | Scope    | Timeout  |
| ------------------------------- | ---------------------------- | --------------- | -------- | -------- |
| Scope                           | ubuntu-26.04                 |                 | every    | 3        |
| Linux x86-64                    | ubuntu-26.04                 | Scope           | fast     | 25       |
| Policy and formatting           | ubuntu-26.04                 | Scope           | fast     | 10       |
| `clang-tidy` × 3 shards         | ubuntu-26.04                 | Scope           | full     | 25       |
| Linux AArch64                   | ubuntu-26.04-arm             | Scope           | full     | 25       |
| macOS AArch64                   | macos-26                     | Scope           | full     | 30       |
| macOS x86-64                    | macos-26, under Rosetta      | Scope           | full     | 35       |
| Windows x86-64                  | windows-2025                 | Scope           | full     | 30       |
| Windows AArch64                 | windows-11-arm               | Scope           | full     | 30       |
| Windows x86-64 under emulation  | windows-11-arm               | Windows x86-64  | extended | 30       |
| FreeBSD image                   | ubuntu-26.04, KVM guest      | Scope           | full     | 30       |
| FreeBSD x86-64                  | ubuntu-26.04, KVM guest      | FreeBSD image   | full     | 45       |
| FreeBSD AArch64                 | ubuntu-26.04, KVM guest      | FreeBSD image   | full     | 45       |
| FreeBSD AArch64 under emulation | ubuntu-26.04, emulated guest | FreeBSD AArch64 | full     | 60 / 180 |
| FreeBSD transfer                | ubuntu-26.04, emulated guest | FreeBSD x86-64  | extended | 90       |
| `CI` — the gate                 | ubuntu-26.04                 | every job       | every    | 5        |

GitHub may annotate the macOS jobs with a notice that macOS arm64 runners can queue longer under capacity constraints. It is informational: it says nothing about the run it appears on, and no workflow setting removes it.

Five jobs wait for more than `Scope`: the two FreeBSD builds wait for the disk they share, and the three jobs that run under emulation each wait for the build whose output they download. Build and test share one job per target. Splitting them would cost an artifact round-trip and a second runner acquisition on every target without proving anything the closure check below does not prove more directly.

## Required check

Branch protection requires one check: **`CI`**. It aggregates every job of the run, accepting `success` and `skipped`, so a docs-only change and the fast lane satisfy it without every target having run, and any failure anywhere fails it. It is guarded by `if: ${{ !cancelled() }}`, not `if: always()`, so cancelling a run ends it immediately instead of leaving the gate to outlive it.

Each target's check run carries the target's name, for example `Linux x86-64`; the README's platform badges read those names on `dev`. Inside a release the same jobs appear as `CI / Linux x86-64`.

## Cancelling a run

- `CI.yml` sets one concurrency group per branch, so a new push supersedes the run in flight, on `dev` too: the compilation cache is saved before the tests, so a superseded run has already warmed it. A branch push and its pull request share the group, so the same commit is never built twice. A dispatched run is never cancelled.
- Every target is a job of the one run, so one Cancel stops them all, including the guests, which `vmactions/freebsd-vm` shuts down with the job.
- `Release.yml` never cancels itself, and nothing else can cancel it: the concurrency group of `CI.yml` starts with the workflow name, which is the caller's when a release runs those jobs, so a push to the same branch lands in a different group. A half-published release is worse than a wasted runner.

## Toolchains

`.github/Toolchains.env` is the single source of truth for every pinned version and every asset checksum. Workflows load it with one `grep` into `GITHUB_ENV`, the setup scripts and packers parse it, and every toolchain and compilation cache key hashes it, so changing a pin invalidates exactly the caches that must change. The format is flat `KEY=VALUE`: POSIX `sh` dot-sources it and PowerShell parses it with one regex, and because it is sourced, every reader rejects a manifest containing anything but comments and plain assignments before reading it.

Every host job starts with the composite action `.github/actions/toolchain`. It restores one prefix — Clang 23 as `clang++-23`, `clang-format-23` and `clang-tidy-23`, the compiler's resource headers, `llvm-readobj` on Windows, and `ccache` — from the Actions cache, keyed on the manifest and the packers. When the cache holds nothing for that key, `.github/Scripts/SetupToolchain.sh` (or `.ps1`) packs the prefix from the pinned upstream assets and the action saves it at once, so a build that fails later in the job does not cost the next run a repack. No job fills the cache ahead of the others: jobs that share a prefix all pack it when the cache is cold, which happens once per manifest or packer change, the first to finish saves it, and the rest only see their save refused. That costs a few duplicate downloads on a rare run instead of a serial job in front of every run. The sources:

- **Linux and Windows** — the official LLVM release archives, whose members are listed once and extracted only where needed, plus ccache's static release. Every download is verified against the SHA-256 the publisher recorded; a checksum still recorded as `TBD` stops the packer before it fetches anything.
- **macOS** — LLVM publishes no macOS archive, so the packer installs Homebrew's `llvm@23`, copies the tools out with every library they load, rewrites their install names relative to the prefix, and re-signs them. Homebrew follows every LLVM patch release and keeps no older bottle, so this prefix is pinned by major only: the packer accepts whichever 23.x it gets and records it in the prefix's `MANIFEST`, and a patch release needs no change here. `llvm@23` is an alias of the rolling `llvm` formula while 23 is current and a formula of its own afterwards, so the name stays right across the next major. There is no macOS x86-64 toolchain: that compiler is cross-built from this prefix on the AArch64 runner, and its tests run under Rosetta on the same machine, because the macOS SDK is universal.
- **FreeBSD** — the guests install `llvm23`, `cmake-core`, `ninja`, `ccache4`, and `git-lite` from pkg's `latest` branch, the only one that carries `llvm23`, and the prepared disk is cached (see below).

CMake and Ninja are never downloaded: every runner image ships versions inside the range `CMakeLists.txt` accepts (`3.31...4.4`), and so does FreeBSD's package. The setup scripts check both and name the runner image if one regresses.

The `-23`-suffixed tool names are mandatory, not cosmetic: they are what `Scripts/RepositoryMessages.sh` and `Run.ps1` probe first, and `clang-format` and `clang-tidy` have no `--compiler`-style override. The FreeBSD package spells the compiler `clang++23`, which the same discovery accepts.

## Caches

| Cache                 | Key                                                                  | Approximate size |
| --------------------- | -------------------------------------------------------------------- | ---------------- |
| Toolchain prefix      | `toolchain-<target>-<hash of Toolchains.env and the packers>`        | 110–220 MB × 5   |
| Compilation cache     | `ccache-<target>-<hash of Toolchains.env>-<sha>`, restored by prefix | ≤ 400 MB × 8     |
| Prepared FreeBSD disk | managed by `vmactions/freebsd-vm`, keyed on the prepare script       | ~2 GB × 1        |

The compilation cache uses the separate restore and save actions. The `<sha>` suffix makes its primary key always miss, so every saving run writes back a cache warmer than the one it restored. It is saved on pushes only — a pull request rebuilds the commit its branch push already cached, so letting it save as well would only churn the quota — and **before** the tests run, so a slow or failing suite does not cost the next run its warm cache. In the FreeBSD guests the cache rides in and out with the workspace.

`CCACHE_NOHASHDIR=1` is set so that runs building the same tree at different paths share entries. `RUX_USE_PCH` is off in CI, because a compilation cache and precompiled headers defeat each other.

The prepared FreeBSD disk is the one cache a job fills ahead of its users. The prepare script is written once, in the `FreeBSD image` job, and both FreeBSD build jobs alias it and share one boot step, so the three guests cannot disagree about the disk; `Tests/Scripts/CI/Check.sh` refuses a second copy. `FreeBSD image` runs first, copies nothing back, and is the only job that ever saves the disk: two build jobs saving it at once made the loser wait on an upload that was discarded, and the action's copy-back gave up on that wait with a `Background tasks timed out` warning. On a warm cache it boots the cached disk and exits.

## FreeBSD

There is no hosted FreeBSD runner, and no hosted runner accelerates an AArch64 guest: the Linux ARM runners expose no KVM and the macOS runners no nested virtualization. Both FreeBSD targets therefore run in `vmactions/freebsd-vm` guests on x86-64 Linux hosts, pinned to a commit.

- **FreeBSD x86-64** runs natively under KVM. The guest is prepared once — the `latest` package branch, the packages above, and the AArch64 base system unpacked as a sysroot — and cached; later runs boot straight into it. The workspace is mirrored at the host's path, each step runs through the guest shell and is synced back, and `.github/Scripts/FreeBSDEnv.sh` exports inside the guest what the toolchain action exports on a host.
- **FreeBSD AArch64** is cross-compiled in that same x86-64 guest, with the guest's own Clang and lld against the sysroot, through `Compiler/CMake/Toolchains/FreeBSD-AArch64.cmake`, without the C++ unit tests, which no machine in the run could execute (`RUX_BUILD_TESTS=OFF`, which `Verify.sh` turns into `Run.sh build --no-tests`); the host then checks the result is a FreeBSD AArch64 ELF image before uploading it. A separate AArch64 guest with no toolchain installed downloads the compiler and runs it under emulation: in the full scope a bounded smoke — every workspace package checked, the native fixtures, the runtime closure, and one language test built and launched — and in the extended scope the lint and the complete Rux test suite as well. The C++ unit tests are host-agnostic and already run on the seven other hosts, FreeBSD x86-64 included, so they are not repeated under emulation.
- **The transfer test** is a two-machine contract in the extended scope: the x86-64 guest produces a payload of target binaries and a manifest, and a fresh AArch64 guest with no compiler installed verifies the hashes and ELF identity and executes them.

## Runtime closure

Every target asserts that the built compiler links only against system libraries — `ldd` on Linux and FreeBSD, `otool -L` on macOS, `dumpbin /dependents` on Windows. A toolchain library that is present on the runner is not present on a user's machine, and this is much cheaper to catch here than in a bug report.

## Release

`Release.yml` is dispatched manually with the version to release. It verifies that the version matches `CMakeLists.txt`, that `CHANGELOG.md` has a matching section, and that the tag does not already exist; builds all eight targets by calling `CI.yml` with the `extended` scope; packages each one; builds the Windows MSI; writes `SHA256SUMS`; and creates the tag and a **draft** release. `contents: write` is granted only to the job that tags and drafts.

`dry-run: true` performs everything except the tag and the draft, so a release can be rehearsed in full.

The published asset names are the ones the end-user installers resolve (`Packaging/Linux/install.sh` and `Packaging/Windows/PowerShell/install.ps1`), including the unqualified compatibility aliases.

## Scripts

Everything under `.github` is PowerShell or POSIX shell. There is no Python.

| Script                                                      | Role                                                           |
| ----------------------------------------------------------- | -------------------------------------------------------------- |
| `actions/toolchain/action.yml`                              | Restore or pack the host toolchain prefix, cache it, export it |
| `Scripts/SetupToolchain.sh` / `.ps1`                        | Pack on a miss, check CMake and Ninja, export the environment  |
| `Scripts/Toolchain/Pack{Linux,MacOS}.sh`, `PackWindows.ps1` | Write one prefix from the pinned upstream assets               |
| `Scripts/VsDevEnv.ps1`                                      | Import the Visual Studio environment once and export it        |
| `Scripts/Scope.sh`                                          | Classify the changed files and resolve the verification scope  |
| `Scripts/FreeBSDEnv.sh`                                     | Export the build environment inside a FreeBSD guest            |
| `Scripts/Verify.sh` / `Verify.ps1`                          | Build, test, format, and closure stages                        |

`Verify.sh` runs unchanged on a native POSIX runner and inside the FreeBSD guest, so those two paths cannot drift. Both verifiers go through `./Run.sh` and `./Run.ps1`, the entry points developers use, so a green job means the developer workflow is green.

## Local equivalents

```sh
sh .github/Scripts/SetupToolchain.sh --target linux-x86_64   # or linux-aarch64, macos-aarch64
sh Run.sh test --clang-tidy
```

```powershell
./.github/Scripts/SetupToolchain.ps1 -Target windows-x86_64   # or windows-aarch64
./Run.ps1 test -ClangTidy
```

Local development does not need the packed prefix: any Clang 23.1+, CMake 3.31+, and Ninja 1.13.2+ installed the way `Docs/Platforms/` describes will do. The prefix exists so CI installs nothing.
