<p align="center">
  <a href="https://rux-lang.dev">
    <img src="https://rux-lang.dev/logo.svg" alt="Rux logo" width="150">
  </a>
</p>

# Rux Programming Language

Rux is a fast, compiled, strongly typed, multi-paradigm programming language.

[![FreeBSD](https://github.com/rux-lang/Rux/actions/workflows/FreeBSD.yml/badge.svg)](https://github.com/rux-lang/Rux/actions/workflows/FreeBSD.yml)
[![Linux](https://github.com/rux-lang/Rux/actions/workflows/Linux.yml/badge.svg)](https://github.com/rux-lang/Rux/actions/workflows/Linux.yml)
[![macOS](https://github.com/rux-lang/Rux/actions/workflows/macOS.yml/badge.svg)](https://github.com/rux-lang/Rux/actions/workflows/macOS.yml)
[![Windows](https://github.com/rux-lang/Rux/actions/workflows/Windows.yml/badge.svg)](https://github.com/rux-lang/Rux/actions/workflows/Windows.yml)
[![Code Quality](https://github.com/rux-lang/Rux/actions/workflows/CodeQuality.yml/badge.svg)](https://github.com/rux-lang/Rux/actions/workflows/CodeQuality.yml)
[![Release](https://img.shields.io/github/v/release/rux-lang/Rux?style=flat&logo=github&label=Release&color=green)](https://github.com/rux-lang/Rux/releases)
[![License](https://img.shields.io/github/license/rux-lang/Rux?style=flat)](LICENSE.md)

> [!IMPORTANT]
> Rux is under active, pre-1.0 development. Language features, compiler behavior, and package formats may change between minor releases. Check the [changelog](CHANGELOG.md) when upgrading.

## Documentation

- [Get started](https://rux-lang.dev/start)
- [Language reference](https://rux-lang.dev/docs)
- [API reference](https://rux-lang.dev/api)
- [CLI reference](https://rux-lang.dev/cli)

Platform installation and source-build guides are indexed in the next two sections. For work on the compiler and repository, use the guide that matches the task:

- [Contributing](CONTRIBUTING.md) — prepare a change and understand the contribution requirements.
- [Development Workflow](Docs/Workflow.md) — configure a development build, run tests, format code, or find a component.
- [Compiler Architecture](Docs/Architecture.md) — understand component ownership, dependency direction, or the compilation pipeline.
- [First-Party Packages](Docs/Packages.md) — review package status, layout, dependencies, and centralized tests.
- [Branch Architecture](Docs/Branches.md) — create a topic branch or understand how changes reach a release.
- [Pull Request Lifecycle](Docs/PullRequest.md) — prepare, review, update, or merge a pull request.
- [CI/CD Flow](Docs/CI-CD.md) — reproduce a required check or understand platform-specific CI.
- [Release Pipeline](Docs/Release.md) — prepare a version, tag it, verify artifacts, and publish a release.

Installer implementation and maintenance documentation lives beside its source code:

- [Linux installer](Packaging/Linux/README.md)
- [Windows installers](Packaging/Windows/README.md)

These are living documents. Update the relevant page in the same pull request whenever a process, branch rule, installer, or workflow changes. Commands use POSIX shell syntax unless a PowerShell example is provided; on Windows, replace `./Bin/rux` with `.\Bin\rux.exe`.

## Installing a Release

Prebuilt x86-64 releases are currently published for Linux and Windows. Choose your operating system for installation, upgrade, removal, and verification instructions:

- [FreeBSD](Docs/Platforms/FreeBSD.md#installing-a-release) — build from source; a native release is not published yet.
- [Linux](Docs/Platforms/Linux.md#installing-a-release) — install with the shell installer.
- [macOS](Docs/Platforms/macOS.md#installing-a-release) — build from source; a native release is not published yet.
- [Windows](Docs/Platforms/Windows.md#installing-a-release) — install with PowerShell, Scoop, or MSI.

## Building from Source

Rux is written in C++26 and currently builds with Clang. GCC and MSVC support is planned but not yet available. Choose your operating system for prerequisites, build commands, and verification steps:

- [FreeBSD](Docs/Platforms/FreeBSD.md#building-from-source)
- [Linux](Docs/Platforms/Linux.md#building-from-source)
- [macOS](Docs/Platforms/macOS.md#building-from-source)
- [Windows](Docs/Platforms/Windows.md#building-from-source)

## Contributing

Contributions are welcome. Start with the [contributing guide](CONTRIBUTING.md) and open pull requests against the `dev` branch. The [development workflow](Docs/Workflow.md) explains the compiler layout, tests, formatting, and static analysis in detail.

## Community

Join the project on [GitHub Discussions](https://github.com/rux-lang/Rux/discussions), [Discord](https://discord.com/invite/uvSHjtZSVG), [Reddit](https://www.reddit.com/r/ruxlang), [YouTube](https://www.youtube.com/@ruxlang), [Bluesky](https://bsky.app/profile/rux-lang.dev), [Mastodon](https://mastodon.social/@ruxlang), or [Telegram](https://t.me/ruxlang). More links are available on the [community page](https://rux-lang.dev/community).

## License

Rux is available under the [MIT License](LICENSE.md).
