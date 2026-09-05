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

   PowerShell users can run `./Run.ps1 test` for the complete build-and-test workflow, or `./Run.ps1 test -SkipBuild` after an existing build. Linux, macOS, and FreeBSD users can run `sh Run.sh test`, or `sh Run.sh test --skip-build` after an existing build. Add `-ClangTidy` or `--clang-tidy` before submitting compiler changes to run the slower static-analysis pass locally; CI always enforces it.

4. Format all maintained C++ and Rux source files with `./Run.ps1 format` on PowerShell or `sh Run.sh format` on Linux, macOS, and FreeBSD.
5. Push your branch and open a Pull Request **against `dev`**, filling in the [pull request template](.github/PULL_REQUEST_TEMPLATE.md).

## Process Documentation

Compiler component ownership is described in [Architecture](Docs/Architecture.md). See the workflow guide's [build and test throughput](Docs/Workflow.md#build-and-test-throughput) section for stable build metadata, compilation launchers, PCH, ThinLTO, and repeatable measurements. Repository verification uses up to four test workers; `rux test --jobs N` defaults to one worker when invoked directly.

For anything beyond the quick start, see the detailed guides:

| Guide                                             | What it covers                                                  |
| ------------------------------------------------- | --------------------------------------------------------------- |
| [Comments and Documentation](Docs/Comments.md)    | Comment syntax, doc attachment, tags, formatting, and tooling   |
| [Language Ownership](Docs/Language.md)            | Values, references, copy, move, construction, and destruction   |
| [Development Workflow](Docs/Workflow.md)          | Day-to-day loop: build, change, test, format, commit            |
| [Compiler Architecture](Docs/Architecture.md)     | Component ownership, dependency direction, compilation pipeline |
| [Package Builds](Docs/Builds.md)                  | Profiles, targets, artifact paths, and the 16-cell build matrix |
| [Rux.toml Manifest](Docs/Manifest.md)             | Versioned package, workspace and dependency contract            |
| [First-Party Packages](Docs/Packages.md)          | Package status, layout, dependencies, and centralized tests     |
| [Known Compiler Defects](Docs/CompilerDefects.md) | Defects that shape how the first-party packages are written     |
| [Cryptographic Review](Docs/CryptoReview.md)      | What an independent reviewer of `Rux/Crypto` needs first        |
| [Branch Architecture](Docs/Branches.md)           | What `main` and `dev` are for, naming, protection rules         |
| [Pull Request Lifecycle](Docs/PullRequest.md)     | From opening a PR to merge: review, CI gates, etiquette         |
| [CI/CD Flow](Docs/CI-CD.md)                       | The per-OS build/test workflows that run on every push and PR   |
| [Release Pipeline](Docs/Release.md)               | How a tag becomes a published, multi-platform release           |

## Code Style

Formatting is enforced by [`.clang-format`](.clang-format) for C++ (LLVM base, 4-space indent, west const, 120-column limit) and by `rux fmt` for Rux sources. Format all maintained source files before committing:

```sh
sh Run.sh format
```

PowerShell equivalent:

```powershell
./Run.ps1 format
```

To check formatting without modifying files:

```sh
sh Run.sh format --check
```

```powershell
./Run.ps1 format -Check
```

The command covers `Compiler/`, maintained C++ unit-test code, and every Rux package and executable-test source tree. Vendored C++ and intentionally malformed golden diagnostic fixtures are excluded. `sh Run.sh tidy` or `./Run.ps1 tidy` additionally checks every maintained translation unit in the CMake compilation database with Clang 23's `clang-tidy`. Otherwise, match the conventions already in the codebase — consistency matters more than personal preference.

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

Source-tree policy checks only host API isolation, internal code generation/linking and CLI output ownership. Repository-command and installer behavior checks are ordinary CTest tests. File length is a review guideline: prefer focused files near 800–1,000 lines and review ownership near 1,200 lines; no automated line-count gate runs locally or in CI.

Repository test, tidy, and format commands default to one worker per available processor. Override with `./Run.ps1 test -Jobs 2` or `sh Run.sh test --jobs 2`. Direct `rux test` remains serial unless passed `--jobs N`. Follow the [build and test throughput](Docs/Workflow.md#build-and-test-throughput) guidance when changing build settings, header ownership, or compilation hot paths. Use LF for all maintained text files.
