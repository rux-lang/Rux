# CI/CD

Continuous integration builds and verifies Rux on all eight supported targets using prebuilt toolchains. No CI job compiles Clang, CMake, Ninja, or Git, and no job installs a target toolchain: Rux emits and links every target format in-process.

## Workflows

Three workflows, plus the community metadata GitHub owns.

- **`Ci.yml`** — the only workflow that runs on a push or a pull request. It plans the scope, runs the host-independent checks once, and calls `Build.yml` for the targets it selected. It also carries the branch policy that rejects pull requests targeting `main`.
- **`Build.yml`** — a reusable workflow (`workflow_call`) describing how one target is built and verified. It is the single description of that work; `Ci.yml` and `Release.yml` both call it, so a platform change is made once.
- **`Release.yml`** — manual only (`workflow_dispatch`, with a version input and a dry-run switch). Pushing a tag never starts a release by itself.

`Build.yml` takes a JSON array of target ids. Rows in its matrix `include` whose target is not in that array are not expanded into jobs, so passing `'["linux-x86_64"]'` produces exactly one job on exactly the right runner.

## Scope

Verification is tiered, so an ordinary push gets an answer quickly and anything that can reach `dev` gets the full matrix.

| Trigger                         | Targets                                                              |
| ------------------------------- | -------------------------------------------------------------------- |
| Push to a topic branch          | `linux-x86_64`, plus the quality checks                              |
| Pull request to `dev` or `main` | all eight, plus cross execution and clang-tidy                       |
| Push to `dev`                   | all eight; this is what warms the caches every other branch restores |
| Pull request labelled `ci:full` | all eight                                                            |
| Nightly (03:00 UTC)             | all eight                                                            |
| `workflow_dispatch`             | whichever scope you choose                                           |

The `plan` job decides this. Scope selection deliberately lives in a job rather than in a trigger-level `paths-ignore` or GitHub's native `[skip ci]` handling: a workflow that never starts never reports its required check, which would leave a pull request blocked forever. For the same reason `[skip ci]` in a commit subject is honoured only on a topic-branch push; on a pull request it is reported as a warning and ignored. A change that touches only prose selects an empty target list, so every build job is skipped while the gate still reports.

## Jobs and timeouts

Every job carries an explicit timeout. Nothing can run for hours.

| Job                                          | Runner                          | Timeout |
| -------------------------------------------- | ------------------------------- | ------- |
| `Plan`                                       | ubuntu-26.04                    | 5       |
| `Quality` — policy guards and C++ formatting | ubuntu-26.04                    | 10      |
| `clang-tidy` × 3 shards                      | ubuntu-26.04                    | 25      |
| Linux x86-64 / AArch64                       | ubuntu-26.04 / ubuntu-26.04-arm | 20      |
| macOS AArch64 / x86-64                       | macos-26 (both)                 | 25 / 30 |
| Windows x86-64 / AArch64                     | windows-2025 / windows-11-arm   | 30      |
| FreeBSD x86-64                               | ubuntu-26.04 (KVM guest)        | 35      |
| FreeBSD AArch64                              | ubuntu-26.04-arm                | 60      |
| FreeBSD transferred artifact                 | ubuntu-26.04                    | 40      |
| Cross execution under emulation              | windows-11-arm                  | 20      |
| `CI` — the aggregate gate                    | ubuntu-26.04                    | 5       |

Build and test share one job per target. Splitting them cost an artifact round-trip and a second runner acquisition on every target without proving anything the closure check below does not prove more directly.

## Required checks

Branch protection requires exactly one check: **`CI`**. It is an aggregate that accepts `success` and `skipped` from every other job, which is what lets the fast lane pass with seven targets skipped and a docs-only change pass with all of them skipped.

It is guarded by `if: ${{ !cancelled() }}`, not `if: always()`. Cancelling a run therefore ends it immediately instead of leaving the gate to outlive it.

## Cancelling a run

- `Ci.yml` sets `cancel-in-progress`, so a new push supersedes the previous run. A branch push and its pull request share one concurrency group, so the same commit is never built twice. Runs on `dev` are exempt, because they are what warms the caches.
- Jobs called through `workflow_call` run inside the caller's run, so one Cancel stops every target.
- `.github/Scripts/FreeBSDVM.sh` traps `INT`, `TERM`, and `EXIT` and kills the guest, so a cancelled FreeBSD job stops in seconds rather than waiting out its timeout. The trap only kills; result collection happens on the normal path, so a cancellation cannot hang in `rsync`.
- `Release.yml` never cancels itself. A half-published release is worse than a wasted runner.

## Toolchains

`.github/Toolchains.env` is the single source of truth for every pinned version and every asset checksum. Workflows load it with one `grep` into `GITHUB_ENV`, both setup scripts parse it, and every cache key hashes it, so bumping `TOOLCHAIN_REVISION` invalidates exactly the caches that must change.

The format is flat `KEY=VALUE`. POSIX `sh` dot-sources it directly and PowerShell parses it with one regex, so no JSON parser and no `jq` is needed on any runner — including the FreeBSD guest. Because it is sourced, both setup scripts reject a manifest containing anything but comments and plain assignments before reading it.

Assets are published by **`rux-lang/Toolchain`** under the tag `toolchain-<revision>`:

- Five host bundles, `rux-toolchain-<target>-<revision>.tar.zst` (`.zip` on Windows), each containing Clang 23 (`clang++-23`, `clang-format-23`, `clang-tidy-23`), CMake, Ninja, and ccache. They are repacked from upstream prebuilt distributions; LLVM is never compiled. There is no macOS x86-64 bundle: LLVM publishes no macOS archive for 23.1.0 and Homebrew has no Intel bottle for macOS 26, so that target is cross-built on the AArch64 runner with the AArch64 bundle and its tests run under Rosetta on the same machine. The macOS SDK is universal, so this needs no Intel toolchain and no Intel runner.
- Five prepared FreeBSD 15.1 guest images, zstd-compressed and split into release-asset-sized parts: `build` and `runtime` for each architecture, plus a minimal AArch64 image that deliberately contains no LLVM, CMake, Ninja, or Git. That emptiness is what makes the transferred-artifact acceptance mean something.

CMake 4.4.3 is not packaged for FreeBSD 15.1 on either architecture, so the toolchains repository builds it once per revision and bakes it into the images. Rux CI never builds it.

The `-23`-suffixed tool names are mandatory, not cosmetic: they are what `Scripts/RepositoryMessages.sh` and `Run.ps1` probe first, and `clang-format` and `clang-tidy` have no `--compiler`-style override.

Every download is staged into a `.partial` file and renamed only after its SHA-256 matches, so an interrupted download can never be mistaken for a verified one. A checksum still recorded as `TBD` is a hard failure, never a fetch.

## Caches

| Cache             | Key                                                    | Approximate size |
| ----------------- | ------------------------------------------------------ | ---------------- |
| Toolchain bundle  | `toolchain-<revision>-<target>`                        | ~200 MB × 5      |
| Compilation cache | `ccache-<target>-<revision>-<sha>`, restored by prefix | 400 MB × 8       |

The `<sha>` suffix makes the primary key always miss, so every run writes back a cache warmer than the one it restored. Caches are saved on pushes only; a pull request rebuilds the commit its branch push already cached, so letting it save as well would only churn the quota. The compilation cache is saved **before** the tests run, so a slow or failing suite does not cost the next run its warm cache — except on FreeBSD, where build and test share one guest boot.

`CCACHE_NOHASHDIR=1` is set because the FreeBSD guest builds under `/root/rux` while a host builds under the workspace path; without it the two would never share entries. `RUX_USE_PCH` is off in CI, because a compilation cache and precompiled headers defeat each other.

FreeBSD guest images are deliberately **not** cached. Downloading them from the release CDN is not slower than restoring them, and it leaves the quota to the compilation caches that actually benefit.

> Actions caches are visible only to the branch that created them and to the repository's default branch. Keeping `dev` as the default branch is therefore what lets a topic branch's first push restore a warm cache instead of paying for a cold build.

## FreeBSD

There is no hosted FreeBSD runner, so both FreeBSD targets run inside QEMU guests booted from the prepared images by `.github/Scripts/FreeBSDVM.sh`. The driver resolves and verifies the image, boots it, waits for SSH, asserts the guest's recorded revision, syncs the tree in, runs the command, and syncs artifacts back — including when the command failed, so a failing job still yields its logs and a warmed cache.

The x86-64 guest is KVM-accelerated on an x86-64 host and refuses to run without it. The AArch64 guest runs on `ubuntu-26.04-arm` so that it is accelerated if that runner exposes `/dev/kvm`, and falls back to TCG emulation otherwise, which is no slower there than anywhere else.

The transferred-artifact acceptance is a two-machine contract: an x86-64 guest produces a payload of target binaries and a manifest, and a _fresh_ AArch64 guest with no compiler installed verifies the hashes and ELF identity and executes them.

## Runtime closure

Every target asserts that the built compiler links only against system libraries — `ldd` on Linux and FreeBSD, `otool -L` on macOS, `dumpbin /dependents` on Windows. A toolchain library that is present on the runner is not present on a user's machine, and this is much cheaper to catch here than in a bug report.

## Release

`Release.yml` is dispatched manually with the version to release. It verifies that the version matches `CMakeLists.txt`, that `CHANGELOG.md` has a matching section, and that the tag does not already exist; builds all eight targets through the same `Build.yml` that CI uses; packages each one; builds the Windows
MSI; writes `SHA256SUMS`; and creates the tag and a **draft** release. `contents: write` is granted only to the publishing job.

`dry-run: true` performs everything except the tag and the draft, so a release can be rehearsed in full.

The published asset names are the ones the end-user installers resolve (`Packaging/Linux/install.sh` and `Packaging/Windows/PowerShell/install.ps1`), including the unqualified compatibility aliases.

## Scripts

Everything under `.github` is PowerShell or POSIX shell. There is no Python.

| Script                               | Role                                                    |
| ------------------------------------ | ------------------------------------------------------- |
| `Scripts/SetupToolchain.sh` / `.ps1` | Restore, verify, and export one host toolchain bundle   |
| `Scripts/VsDevEnv.ps1`               | Import the Visual Studio environment once and export it |
| `Scripts/FreeBSDVM.sh`               | Boot a prepared guest, run a command, sync results back |
| `Scripts/Verify.sh` / `Verify.ps1`   | Build, test, format, and closure stages                 |

`Verify.sh` runs unchanged on a native POSIX runner and inside the FreeBSD guest, so those two paths cannot drift. Both verifiers go through `./Run.sh` and `./Run.ps1`, the entry points developers use, so a green job means the developer workflow is green.

`Tests/Scripts/CI/Check.sh` covers these helpers as the CTest test `Scripts.CI`. It runs on every host: there is no optional interpreter for the coverage to disappear behind.

## Local equivalents

```sh
sh .github/Scripts/SetupToolchain.sh --target linux-x86_64   # or macos-aarch64, …
sh Run.sh test --clang-tidy
```

```powershell
./.github/Scripts/VsDevEnv.ps1 -Arch amd64   # arm64 on an AArch64 host
./Run.ps1 test -ClangTidy
```

Local development does not need the published bundle: any Clang 23.1+, CMake 4.4.3+, and Ninja 1.13.2+ installed the way `Docs/Platforms/` describes will do. The bundle exists so CI installs nothing.
