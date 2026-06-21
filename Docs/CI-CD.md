# CI/CD Flow

Continuous integration runs on every push and pull request to `main` and `dev`,
building and testing Rux across every supported operating system. Continuous
delivery (publishing binaries) is covered separately in the
[Release Pipeline](Release.md).

## Per-OS build & test workflows

Each supported platform has its own workflow under
[`.github/workflows/`](../.github/workflows/):

| Workflow        | Platform            |
|-----------------|---------------------|
| `ubuntu.yml`    | Ubuntu 24.04        |
| `windows.yml`   | Windows Server 2025 |
| `macos.yml`     | macOS 26            |
| `freebsd.yml`   | FreeBSD 14.2        |
| `dragonfly.yml` | DragonFly BSD 6.4   |
| `netbsd.yml`    | NetBSD 10.1         |
| `openbsd.yml`   | OpenBSD 7.6         |
| `omnios.yml`    | OmniOS r151052      |

Their status is shown by the badges at the top of the [README](../README.md).

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

Using `ubuntu.yml` as the reference shape (other OSes differ only in how the
toolchain is installed):

1. **Build job**
    - Check out the repo.
    - Install Clang 22, plus CMake and Ninja.
    - Configure and build Release:
      ```sh
      cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++-22
      cmake --build build --config Release
      ```
    - Upload the `rux` binary as an artifact.
2. **Test job** (`needs: build`)
    - Download the built binary and restore its executable bit.
    - `./build/rux install Std` (test packages depend on Std).
    - `./build/rux test --release` from the repo root (workspace mode discovers `Tests/`).

> _TODO: note any platform-specific quirks (e.g. Windows initializes the Visual
> Studio dev environment before configuring; macOS installs LLVM from Homebrew
> because Apple Clang lacks full C++26 support)._

## Required checks

> _TODO: list which of these workflows are *required* to pass before a PR can
> merge, as configured in branch protection (see
> [Branch Architecture](Branches.md))._

## Reproducing CI locally

The CI build is the same `cmake` + `rux test` flow documented in the
[Development Workflow](Workflow.md). To match CI exactly, build
`Release` and run `rux test --release`.

> _TODO: document caching, runner images, or self-hosted runners if any are
> introduced._
