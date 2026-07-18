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
   cmake -S . -B build -DRUX_BUILD_TESTS=ON
   cmake --build build --config Release
   ```
2. [Fork](https://github.com/rux-lang/Rux/fork) the repo and branch off `dev`:
   ```sh
   git switch dev
   git pull --ff-only
   git switch -c my-feature
   ```
3. Make your change, add a test, and run both suites. Bare `rux install` discovers dependencies from the manifest-less test workspace:
   ```sh
   ./Bin/Release/rux install
   ./Bin/Release/rux test --release
   ctest --test-dir build --output-on-failure -C Release
   ```
   On Windows, use `.\Bin\Release\rux.exe`; the CMake and CTest commands are the same.
4. Format touched files with `clang-format -i <files>`.
5. Push your branch and open a Pull Request **against `dev`**.

## Process Documentation

For anything beyond the quick start, see the detailed guides:

| Guide                                         | What it covers                                                |
| --------------------------------------------- | ------------------------------------------------------------- |
| [Development Workflow](Docs/Workflow.md)      | Day-to-day loop: build, change, test, format, commit          |
| [Branch Architecture](Docs/Branches.md)       | What `main` and `dev` are for, naming, protection rules       |
| [Pull Request Lifecycle](Docs/PullRequest.md) | From opening a PR to merge: review, CI gates, etiquette       |
| [CI/CD Flow](Docs/CI-CD.md)                   | The per-OS build/test workflows that run on every push and PR |
| [Release Pipeline](Docs/Release.md)           | How a tag becomes a published, multi-platform release         |

## Code Style

Formatting is enforced by [`.clang-format`](.clang-format) (LLVM base, 4-space indent, west const, 120-column limit). Format the files you changed before committing:

```sh
clang-format -i <files>
```

Or format every source file at once:

```sh
clang-format -i $(git ls-files '*.cpp' '*.h')
```

PowerShell equivalent:

```powershell
git ls-files '*.cpp' '*.h' | ForEach-Object { clang-format -i $_ }
```

Otherwise, match the conventions already in the codebase — consistency matters more than personal preference.

## Reporting Bugs

Include:

- Rux version / commit hash (`rux version`)
- A minimal reproducer (source file or snippet)
- Expected vs. actual behavior
- Whether the problem reproduces on the latest `dev` branch

## Community

Have questions before diving in? Join us on [Discord](https://discord.com/invite/uvSHjtZSVG) or [GitHub Discussions](https://github.com/rux-lang/Rux/discussions). Please also read our [Code of Conduct](.github/CODE_OF_CONDUCT.md).

## License

By contributing you agree that your work will be licensed under the [MIT License](LICENSE.md).
