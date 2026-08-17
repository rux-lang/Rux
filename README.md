<p align="center">
  <a href="https://rux-lang.dev">
    <img src="https://rux-lang.dev/logo.svg" alt="Rux logo" width="80">
  </a>
</p>

# Rux Programming Language

[![FreeBSD](https://github.com/rux-lang/Rux/actions/workflows/FreeBSD.yml/badge.svg)](https://github.com/rux-lang/Rux/actions/workflows/FreeBSD.yml)
[![Linux](https://github.com/rux-lang/Rux/actions/workflows/Linux.yml/badge.svg)](https://github.com/rux-lang/Rux/actions/workflows/Linux.yml)
[![macOS](https://github.com/rux-lang/Rux/actions/workflows/macOS.yml/badge.svg)](https://github.com/rux-lang/Rux/actions/workflows/macOS.yml)
[![Windows](https://github.com/rux-lang/Rux/actions/workflows/Windows.yml/badge.svg)](https://github.com/rux-lang/Rux/actions/workflows/Windows.yml)
[![Code Quality](https://github.com/rux-lang/Rux/actions/workflows/CodeQuality.yml/badge.svg)](https://github.com/rux-lang/Rux/actions/workflows/CodeQuality.yml)
[![Release](https://img.shields.io/github/v/release/rux-lang/Rux?style=flat&logo=github&label=Release&color=green)](https://github.com/rux-lang/Rux/releases)
[![License](https://img.shields.io/github/license/rux-lang/Rux?style=flat)](LICENSE.md)

Rux is a fast, compiled, strongly typed, multi-paradigm programming language. The compiler is self-contained: its own x86-64 and AArch64 code generators, its own object format, and its own PE/ELF/Mach-O linkers produce a native executable with no assembler, no C compiler, and no external linker anywhere in the pipeline. Rux supports four operating systems — FreeBSD, Linux, macOS and Windows — on x86-64 and AArch64.

Debug builds preserve the unoptimized program structure, while Release builds use explicit, bounded HIR and LIR passes and remove unreachable private declarations before either native back end runs. Shared RCU module construction and a format-neutral link graph keep object and image behavior consistent across all eight targets.

The platform badges cover native x86-64 and AArch64 builds and tests. The [CI/CD guide](Docs/CI-CD.md) documents the native, cross-compiler, transferred-artifact, and runtime acceptance paths in detail.

> [!IMPORTANT]
> Rux is under active, pre-1.0 development. Language features, compiler behavior, and package formats may change between minor releases. Check the [changelog](CHANGELOG.md) when upgrading.

## Documentation

Language and tool documentation lives on the website:

- [Get started](https://rux-lang.dev/docs/start)
- [Rux reference](https://rux-lang.dev/docs/lang)
- [CLI reference](https://rux-lang.dev/docs/cli)
- [API reference](https://rux-lang.dev/docs/api)

Working on the compiler itself is covered by the guides in [`Docs/`](Docs/), indexed in [CONTRIBUTING.md](CONTRIBUTING.md#process-documentation). Installer implementation notes live beside their source, in [Packaging/Linux](Packaging/Linux/README.md) and [Packaging/Windows](Packaging/Windows/README.md).

The [compiler architecture guide](Docs/Architecture.md) documents component ownership, semantic and lowering boundaries, optimization, code generation, and linking. The [package build guide](Docs/Builds.md) covers target names, artifact paths, cross-compilation, and the 16-cell `rux build --all` matrix. Contributors changing diagnostics or CLI output should follow the [user-facing message contract](Docs/Workflow.md#user-facing-message-contract).

Commands in this repository use POSIX shell syntax unless a PowerShell example is provided; on Windows, replace `./Bin/rux` with `.\Bin\rux.exe`.

## Installing a Release

Prebuilt x86-64 and AArch64 releases are published for every supported operating system. Choose your operating system for installation, upgrade, removal, and verification instructions:

- [FreeBSD](Docs/Platforms/FreeBSD.md#installing-a-release) — install the native archive.
- [Linux](Docs/Platforms/Linux.md#installing-a-release) — use the x86-64 shell installer or a native archive.
- [macOS](Docs/Platforms/macOS.md#installing-a-release) — install the native archive.
- [Windows](Docs/Platforms/Windows.md#installing-a-release) — use an x86-64 installer or a native ZIP.

## Building from Source

Rux is written in C++26 and builds exclusively with upstream Clang 22.1 or newer. Choose your operating system for prerequisites, build commands, and verification steps:

- [FreeBSD](Docs/Platforms/FreeBSD.md#building-from-source)
- [Linux](Docs/Platforms/Linux.md#building-from-source)
- [macOS](Docs/Platforms/macOS.md#building-from-source)
- [Windows](Docs/Platforms/Windows.md#building-from-source)

Once the prerequisites are installed, one entry point covers local development. `sh Run.sh build` configures and builds the compiler, and `sh Run.sh test` runs the complete verification workflow; `format`, `policy`, `tidy`, `unit`, and `clean` are the individual steps. On Windows, use `.\Run.ps1 build` and `.\Run.ps1 test`. Run the script with no arguments for the full command and option summary.

## Contributing

Contributions are welcome. Start with the [contributing guide](CONTRIBUTING.md) and open pull requests against the `dev` branch. The [development workflow](Docs/Workflow.md) explains the compiler layout, tests, formatting, and static analysis in detail. Documentation is part of the change: update the affected page in the same pull request whenever a process, branch rule, installer, or workflow changes.

## Community

Join the project on [GitHub Discussions](https://github.com/rux-lang/Rux/discussions), [Discord](https://discord.com/invite/uvSHjtZSVG), [Reddit](https://www.reddit.com/r/ruxlang), [YouTube](https://www.youtube.com/@ruxlang), [Bluesky](https://bsky.app/profile/rux-lang.dev), [Mastodon](https://mastodon.social/@ruxlang), or [Telegram](https://t.me/ruxlang). More links are available on the [community page](https://rux-lang.dev/community).

## License

Licensed under the [MIT License](LICENSE.md).
