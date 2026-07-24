# Contributing to Rux

Thanks for your interest in contributing to the Rux programming language! This page is the entry point. It covers the essentials; the deeper process docs live in [`Docs/`](Docs/).

## Ways to Contribute

- Report bugs via [GitHub Issues](https://github.com/rux-lang/Rux/issues)
- Propose language features in [GitHub Discussions](https://github.com/rux-lang/Rux/discussions)
- Submit pull requests for bug fixes or approved features
- Improve compiler and contributor documentation in this repository, or language documentation in [rux-lang/Web](https://github.com/rux-lang/Web)
- [Donate](https://rux-lang.dev/support) to support the project

## Quick Start

1. Build the compiler by following [Building from Source](README.md#building-from-source) in the README, then enable the C++ test target in the same build directory:
   ```sh
   cmake -S . -B Build -G Ninja -DRUX_BUILD_TESTS=ON
   cmake --build Build --config Release
   ```
2. [Fork](https://github.com/rux-lang/Rux/fork) the repo and branch off `dev`:
   ```sh
   git switch dev
   git pull --ff-only
   git switch -c my-feature
   ```
3. Make your change, add a test, and run both suites. Repository tests resolve every dependency from the local workspace, so no registry installation is needed:

   ```sh
   ./Bin/rux test --release
   ctest --test-dir Build --output-on-failure -C Release
   ```

   On Windows, use `.\Bin\rux.exe`; the CMake and CTest commands are the same.

   PowerShell users can run `./Test.ps1` for the complete build-and-test workflow, or `./Test.ps1 -SkipBuild` after an existing build. Linux, macOS, and FreeBSD users can run `sh Test.sh`, or `sh Test.sh --skip-build` after an existing build. Add `-ClangTidy` or `--clang-tidy` before submitting compiler changes to run the slower static-analysis pass locally; CI always enforces it.

4. Format all maintained C++ and Rux source files with `./Format.ps1` on PowerShell or `sh Format.sh` on Linux, macOS, and FreeBSD.
5. Push your branch and open a Pull Request **against `dev`**, filling in the [pull request template](.github/PULL_REQUEST_TEMPLATE.md).

## Process Documentation

For anything beyond the quick start, see the detailed guides:

| Guide                                          | What it covers                                                 |
| ---------------------------------------------- | -------------------------------------------------------------- |
| [Development Workflow](Docs/Workflow.md)       | Day-to-day loop: build, change, test, format, commit           |
| [Compiler Architecture](Docs/Architecture.md)  | Component ownership, dependency direction, compilation pipeline |
| [First-Party Packages](Docs/Packages.md)       | Package status, layout, dependencies, and centralized tests    |
| [Branch Architecture](Docs/Branches.md)        | What `main` and `dev` are for, naming, protection rules        |
| [Pull Request Lifecycle](Docs/PullRequest.md)  | From opening a PR to merge: review, CI gates, etiquette        |
| [CI/CD Flow](Docs/CI-CD.md)                    | The per-OS build/test workflows that run on every push and PR  |
| [Release Pipeline](Docs/Release.md)            | How a tag becomes a published, multi-platform release          |

## Code Style

Formatting is enforced by [`.clang-format`](.clang-format) for C++ (LLVM base, 4-space indent, west const, 120-column limit) and by `rux fmt` for Rux sources. Format all maintained source files before committing:

```sh
sh Format.sh
```

PowerShell equivalent:

```powershell
./Format.ps1
```

To check formatting without modifying files:

```sh
sh Format.sh --check
```

```powershell
./Format.ps1 -Check
```

The scripts cover `Compiler/`, maintained C++ unit-test code, and every Rux package and executable-test source tree. Vendored C++ and intentionally malformed golden diagnostic fixtures are excluded. With `-ClangTidy` or `--clang-tidy`, Clang 22's `clang-tidy` additionally checks every maintained translation unit in the CMake compilation database. Otherwise, match the conventions already in the codebase — consistency matters more than personal preference.

## Reporting Bugs

Open a [bug report](https://github.com/rux-lang/Rux/issues/new?template=bug_report.yml) and fill in the form, which asks for:

- Rux version / commit hash (`rux version`)
- A minimal reproducer (source file or snippet)
- Expected vs. actual behavior
- Operating system and architecture
- Whether the problem reproduces on the latest `dev` branch

Language feature ideas belong in [Discussions](https://github.com/rux-lang/Rux/discussions) first; security issues follow the [security policy](.github/SECURITY.md) and must not be filed as public issues.

## Community

Have questions before diving in? Join us on [Discord](https://discord.com/invite/uvSHjtZSVG) or [GitHub Discussions](https://github.com/rux-lang/Rux/discussions). Please also read our [Code of Conduct](.github/CODE_OF_CONDUCT.md).

## License

By contributing you agree that your work will be licensed under the [MIT License](LICENSE.md).
