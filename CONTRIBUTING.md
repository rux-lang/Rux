# Contributing to Rux

Thanks for your interest in contributing to the Rux programming language! This
page is the entry point. It covers the essentials; the deeper process docs live
in [`Docs/`](Docs/).

## Ways to Contribute

- Report bugs via [GitHub Issues](https://github.com/rux-lang/Rux/issues)
- Propose language features in [GitHub Discussions](https://github.com/rux-lang/Rux/discussions)
- Submit pull requests for bug fixes or approved features
- Improve [documentation](https://github.com/rux-lang/Web)
- [Donate](https://rux-lang.dev/support) to support the project

## Quick Start

1. Build the compiler and run the tests by following
   [Building from Source](README.md#building-from-source) in the README.
2. [Fork](https://github.com/rux-lang/Rux/fork) the repo and branch off `dev`:
   ```sh
   git checkout dev
   git checkout -b my-feature
   ```
3. Make your change, add a test, and run the suite (`./Bin/Release/rux test`).
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

Formatting is enforced by [`.clang-format`](.clang-format) (LLVM base, 4-space
indent, west const, 120-column limit). Format the files you changed before
committing:

```sh
clang-format -i <files>
```

Or format every source file at once:

```sh
clang-format -i $(git ls-files '*.cpp' '*.h')
```

Otherwise, match the conventions already in the codebase — consistency matters
more than personal preference.

## Reporting Bugs

Include:

- Rux version / commit hash (`rux version`)
- A minimal reproducer (source file or snippet)
- Expected vs. actual behavior

## Community

Have questions before diving in? Join us on
[Discord](https://discord.com/invite/uvSHjtZSVG) or
[GitHub Discussions](https://github.com/rux-lang/Rux/discussions). Please also
read our [Code of Conduct](.github/CODE_OF_CONDUCT.md).

## License

By contributing you agree that your work will be licensed under the
[MIT License](LICENSE).
