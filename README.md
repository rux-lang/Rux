<p align="center">
  <a href="https://rux-lang.dev">
    <img src="https://rux-lang.dev/logo.svg" alt="Rux logo" width="150">
  </a>
</p>

# Rux Programming Language

Rux is a fast, compiled, strongly typed, multi-paradigm programming language. On x86-64 the compiler is self-contained — its own code generator, object format, and PE/ELF/Mach-O linkers produce a native executable with no external toolchain; AArch64 targets lower natively and link through the platform Clang driver.

[![FreeBSD](https://github.com/rux-lang/Rux/actions/workflows/FreeBSD.yml/badge.svg)](https://github.com/rux-lang/Rux/actions/workflows/FreeBSD.yml)
[![Linux](https://github.com/rux-lang/Rux/actions/workflows/Linux.yml/badge.svg)](https://github.com/rux-lang/Rux/actions/workflows/Linux.yml)
[![macOS](https://github.com/rux-lang/Rux/actions/workflows/macOS.yml/badge.svg)](https://github.com/rux-lang/Rux/actions/workflows/macOS.yml)
[![Windows](https://github.com/rux-lang/Rux/actions/workflows/Windows.yml/badge.svg)](https://github.com/rux-lang/Rux/actions/workflows/Windows.yml)
[![Code Quality](https://github.com/rux-lang/Rux/actions/workflows/CodeQuality.yml/badge.svg)](https://github.com/rux-lang/Rux/actions/workflows/CodeQuality.yml)
[![Release](https://img.shields.io/github/v/release/rux-lang/Rux?style=flat&logo=github&label=Release&color=green)](https://github.com/rux-lang/Rux/releases)
[![License](https://img.shields.io/github/license/rux-lang/Rux?style=flat)](LICENSE.md)

Each platform badge above covers a native build-and-test run on both supported architectures, x86-64 and AArch64, and prebuilt binaries are published for every one of them.

> [!IMPORTANT]
> Rux is under active, pre-1.0 development. Language features, compiler behavior, and package formats may change between minor releases. Check the [changelog](CHANGELOG.md) when upgrading.

## Hello, Rux

```rux
import Io::PrintLine;
import Rux::{ OperatingSystem, #target };

func Main() -> int {
    let greeting = "Hello, Rux!";

    for i in 0..3 {
        PrintLine(greeting);
    }

    when #target.os == .Windows {
        PrintLine("Compiled for Windows.");
    } else {
        PrintLine("Compiled for a Unix-like system.");
    }

    return 0;
}
```

`when` runs while compiling: only the taken branch is analyzed and emitted, so the other may reference symbols that do not exist on the current target.

Scaffold, build, and run a package with:

```sh
rux new App
cd App
rux run
```

`rux new` writes a minimal `Main`; the sample above also needs `Io` and `Rux` in the manifest's `[Dependencies]`.

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

Rux is available under the [MIT License](LICENSE.md).
