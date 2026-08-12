<p align="center">
  <a href="https://rux-lang.dev">
    <img src="https://rux-lang.dev/logo.svg" alt="Rux logo" width="120">
  </a>
</p>

# Rux Programming Language

Rux is a fast, compiled, strongly typed, multi-paradigm programming language. The compiler is self-contained: its own x86-64 and AArch64 code generators, its own object format, and its own PE/ELF/Mach-O linkers produce a native executable with no assembler, no C compiler, and no external linker anywhere in the pipeline.

Cross-compilation needs no external toolchain: `rux build --target <os>-<arch>` produces another system's artifact from any host, while `rux check --target` performs target analysis without executing it. `rux run` is host-only, and `rux test --target` executes only a same-OS architecture reported for the compiler process or native OS. The x86-64 back end reaches every supported operating system; the AArch64 back end reaches Linux and FreeBSD through ELF, Windows through PE/COFF, and macOS through Mach-O. Other AArch64 System V targets, including OpenBSD, NetBSD, DragonFly BSD, and illumos, remain unsupported.

[![FreeBSD](https://github.com/rux-lang/Rux/actions/workflows/FreeBSD.yml/badge.svg)](https://github.com/rux-lang/Rux/actions/workflows/FreeBSD.yml)
[![Linux](https://github.com/rux-lang/Rux/actions/workflows/Linux.yml/badge.svg)](https://github.com/rux-lang/Rux/actions/workflows/Linux.yml)
[![macOS](https://github.com/rux-lang/Rux/actions/workflows/macOS.yml/badge.svg)](https://github.com/rux-lang/Rux/actions/workflows/macOS.yml)
[![Windows](https://github.com/rux-lang/Rux/actions/workflows/Windows.yml/badge.svg)](https://github.com/rux-lang/Rux/actions/workflows/Windows.yml)
[![Code Quality](https://github.com/rux-lang/Rux/actions/workflows/CodeQuality.yml/badge.svg)](https://github.com/rux-lang/Rux/actions/workflows/CodeQuality.yml)
[![Release](https://img.shields.io/github/v/release/rux-lang/Rux?style=flat&logo=github&label=Release&color=green)](https://github.com/rux-lang/Rux/releases)
[![License](https://img.shields.io/github/license/rux-lang/Rux?style=flat)](LICENSE.md)

Each platform badge above covers a native build-and-test run on both supported architectures, x86-64 and AArch64, and prebuilt binaries are published for every one of them. Linux and Windows also cross-build signed `macos-aarch64` images and inspect their ARM64 Mach-O headers and signatures without launching them. The Windows badge runs the complete native AArch64 suite and a `windows-11-arm` cross job that executes the downloaded x86-64 compiler under Windows translation, then runs its Windows AArch64 output natively, including executable and DLL fixtures. The macOS badge runs the complete Apple Silicon suite and native runtime fixtures, then repeats target tests and fixtures with the x86-64 compiler under Rosetta while every generated ARM64 image executes directly. FreeBSD runs the complete ordinary suite and native AArch64 runtime fixtures, then transfers target-only bytes built by an x86-64 FreeBSD compiler into a fresh AArch64 VM for direct execution.

`freebsd-aarch64` supports executable, shared-library, and static-library
packages from every compiler host without a FreeBSD SDK or sysroot. Foreign
artifacts use the canonical target-separated output directory and must be
transferred to native FreeBSD for execution.

> [!IMPORTANT]
> Rux is under active, pre-1.0 development. Language features, compiler behavior, and package formats may change between minor releases. Check the [changelog](CHANGELOG.md) when upgrading.

## Documentation

Language and tool documentation lives on the website:

- [Get started](https://rux-lang.dev/start)
- [Rux reference](https://rux-lang.dev/docs)
- [CLI reference](https://rux-lang.dev/cli)
- [API reference](https://rux-lang.dev/api)

Working on the compiler itself is covered by the guides in [`Docs/`](Docs/), indexed in [CONTRIBUTING.md](CONTRIBUTING.md#process-documentation). Installer implementation notes live beside their source, in [Packaging/Linux](Packaging/Linux/README.md) and [Packaging/Windows](Packaging/Windows/README.md).

Commands in this repository use POSIX shell syntax unless a PowerShell example is provided; on Windows, replace `./Bin/rux` with `.\Bin\rux.exe`.

## Installing a Release

Prebuilt x86-64 and AArch64 releases are published for every supported operating system. Choose your operating system for installation, upgrade, removal, and verification instructions:

- [FreeBSD](Docs/Platforms/FreeBSD.md#installing-a-release) — install the native archive.
- [Linux](Docs/Platforms/Linux.md#installing-a-release) — use the x86-64 shell installer or a native archive.
- [macOS](Docs/Platforms/macOS.md#installing-a-release) — install the native archive.
- [Windows](Docs/Platforms/Windows.md#installing-a-release) — use an x86-64 installer or a native ZIP.

## Building from Source

Rux is written in C++26 and currently builds with Clang. GCC and MSVC support is planned but not yet available. Choose your operating system for prerequisites, build commands, and verification steps:

- [FreeBSD](Docs/Platforms/FreeBSD.md#building-from-source)
- [Linux](Docs/Platforms/Linux.md#building-from-source)
- [macOS](Docs/Platforms/macOS.md#building-from-source)
- [Windows](Docs/Platforms/Windows.md#building-from-source)

## Contributing

Contributions are welcome. Start with the [contributing guide](CONTRIBUTING.md) and open pull requests against the `dev` branch. The [development workflow](Docs/Workflow.md) explains the compiler layout, tests, formatting, and static analysis in detail. Documentation is part of the change: update the affected page in the same pull request whenever a process, branch rule, installer, or workflow changes.

## Community

Join the project on [GitHub Discussions](https://github.com/rux-lang/Rux/discussions), [Discord](https://discord.com/invite/uvSHjtZSVG), [Reddit](https://www.reddit.com/r/ruxlang), [YouTube](https://www.youtube.com/@ruxlang), [Bluesky](https://bsky.app/profile/rux-lang.dev), [Mastodon](https://mastodon.social/@ruxlang), or [Telegram](https://t.me/ruxlang). More links are available on the [community page](https://rux-lang.dev/community).

## License

Licensed under the [MIT License](LICENSE.md).
