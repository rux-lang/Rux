<p align="center">
  <a href="https://rux-lang.dev" target="_blank">
    <img src="https://rux-lang.dev/logo.svg" alt="Rux Logo" width="150"/>
  </a>
</p>

# Rux Programming Language

[![BSD](https://github.com/rux-lang/Rux/actions/workflows/bsd.yml/badge.svg)](https://github.com/rux-lang/Rux/actions/workflows/bsd.yml)
[![Illumos](https://github.com/rux-lang/Rux/actions/workflows/illumos.yml/badge.svg)](https://github.com/rux-lang/Rux/actions/workflows/illumos.yml)
[![Linux](https://github.com/rux-lang/Rux/actions/workflows/linux.yml/badge.svg)](https://github.com/rux-lang/Rux/actions/workflows/linux.yml)
[![macOS](https://github.com/rux-lang/Rux/actions/workflows/macos.yml/badge.svg)](https://github.com/rux-lang/Rux/actions/workflows/macos.yml)
[![Windows](https://github.com/rux-lang/Rux/actions/workflows/windows.yml/badge.svg)](https://github.com/rux-lang/Rux/actions/workflows/windows.yml)
[![Release](https://img.shields.io/github/v/release/rux-lang/Rux?style=flat&logo=github&label=Release&color=green)](https://github.com/rux-lang/Rux/releases)
[![License](https://img.shields.io/github/license/rux-lang/Rux?style=flat)](https://github.com/rux-lang/Rux/blob/main/LICENSE)

[![GitHub stars](https://img.shields.io/github/stars/rux-lang/Rux?style=flat&logo=github&label=Stars&logoColor=white&color=blue)](https://github.com/rux-lang)
[![GitHub followers](https://img.shields.io/github/followers/rux-lang?style=flat&logo=github&label=Followers&logoColor=white&color=blue)](https://github.com/rux-lang)
[![GitHub forks](https://img.shields.io/github/forks/rux-lang/Rux?style=flat&logo=github&logoColor=white&label=Forks&color=blue)](https://github.com/rux-lang)
[![Discussion](https://img.shields.io/badge/13-gray?style=flat&logo=github&logoColor=white&label=Discussions&color=blue)](https://github.com/rux-lang/Rux/discussions)
[![Discord](https://img.shields.io/discord/1321469752811585576?style=flat&logo=discord&logoColor=white&label=Discord&color=blue)](https://discord.com/invite/uvSHjtZSVG)
[![Reddit](https://img.shields.io/reddit/subreddit-subscribers/ruxlang?style=flat&logo=reddit&logoColor=white&label=Reddit&color=blue)](https://www.reddit.com/r/ruxlang)
[![YouTube](https://img.shields.io/youtube/channel/subscribers/UCNqQ7NIA5pBl3ZO--nOyvDA?style=flat&logo=youtube&logoColor=white&label=YouTube&color=blue)](https://www.youtube.com/@ruxlang)
[![X](https://img.shields.io/badge/76-gray?logo=x&style=flat&logoColor=white&label=Twitter&color=blue)](https://x.com/ruxlang)
[![Bluesky](https://img.shields.io/bluesky/followers/rux-lang.dev?style=flat&logo=Bluesky&logoColor=white&label=Bluesky&color=blue)](https://bsky.app/profile/rux-lang.dev)
[![Mastodon](https://img.shields.io/mastodon/follow/113727153489087809?domain=mastodon.social&style=flat&logo=mastodon&logoColor=white&label=Mastodon&color=blue)](https://mastodon.social/@ruxlang)
[![Telegram](https://img.shields.io/badge/73-gray?style=flat&logo=telegram&logoColor=white&label=Telegram&color=blue)](https://t.me/ruxlang)

---

Rux is a fast, compiled, strongly typed, multi-paradigm programming language.

## Project Status

Currently, under development.

## Documentation

- [Get Started](https://rux-lang.dev/start)
- [Rux Reference](https://rux-lang.dev/docs)
- [API Reference](https://rux-lang.dev/api)
- [CLI Reference](https://rux-lang.dev/cli)

## Building from Source

Rux is written in modern **C++26**, so it needs an up-to-date toolchain. If you are new to building C/C++ projects, just follow the steps for your operating system top to bottom — every command can be copied and pasted.

### Prerequisites

You need the following tools, all in a recent version:

| Tool                              | Version | Description                 |
| --------------------------------- | ------- | --------------------------- |
| [Clang](https://clang.llvm.org/)  | 21.1+   | The C++26 compiler          |
| [CMake](https://cmake.org/)       | 4.2+    | Generates the build files   |
| [Ninja](https://ninja-build.org/) | 1.13+   | Runs the actual build, fast |
| [Git](https://git-scm.com/)       | 2.52+   | Downloads the source code   |

> **Note**
> GCC and MSVC will be supported later — please build with Clang.

### 1. Get the source

```sh
git clone https://github.com/rux-lang/Rux.git
cd Rux
```

### 2. Install the toolchain and configure

Expand the section for your operating system. Each one installs the tools and then runs CMake to prepare a `build` directory.

<details>
<summary><b>DragonFly BSD</b></summary>

```sh
sudo pkg install -y llvm cmake ninja git
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++
```

</details>

<details>
<summary><b>FreeBSD</b></summary>

```sh
sudo pkg install -y llvm22 cmake ninja git
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++22
```

</details>

<details>
<summary><b>macOS</b></summary>

Apple's built-in Clang lags behind and lacks full C++26 support, so install the latest LLVM with [Homebrew](https://brew.sh/):

```sh
brew install llvm cmake ninja git
cmake -S . -B build -G Ninja \
  -DCMAKE_CXX_COMPILER="$(brew --prefix llvm)/bin/clang++"
```

</details>

<details>
<summary><b>NetBSD</b></summary>

```sh
sudo pkgin -y install clang cmake ninja-build git
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++
```

</details>

<details>
<summary><b>OmniOS</b></summary>

```sh
pfexec pkg install clang cmake ninja git
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++
```

</details>

<details>
<summary><b>OpenBSD</b></summary>

```sh
doas pkg_add llvm%22 cmake ninja git
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++
```

</details>

<details>
<summary><b>Ubuntu / Debian</b></summary>

```sh
# Clang (latest) from the official LLVM apt repository
wget https://apt.llvm.org/llvm.sh
chmod +x llvm.sh
sudo ./llvm.sh 22                 # installs clang++-22

# CMake (latest) and Ninja
sudo snap install cmake --classic
sudo apt install -y ninja-build git

cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++-22
```

</details>

<details>
<summary><b>Windows</b></summary>

1. Install **Visual Studio 2026** (the free Community edition is fine) with the **"Desktop development with C++"** workload. This provides the Windows SDK and C runtime that Clang links against.
2. Install the latest Clang, CMake, and Ninja with [Scoop](https://scoop.sh/). If you don't have Scoop yet, install it from a regular (non-admin) PowerShell:

   ```powershell
   Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser
   Invoke-RestMethod -Uri https://get.scoop.sh | Invoke-Expression
   ```

   Then install the tools (all from Scoop's main bucket):

   ```powershell
   scoop install llvm cmake ninja
   ```

3. Open the **"x64 Native Tools Command Prompt for VS 2026"** from the Start menu. This sets up the Windows SDK and C runtime environment that Clang needs to compile and link.

   Prefer your own terminal? Any PowerShell window works too — just initialize the build environment once per session first:

   ```powershell
   $vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
   & "$vs\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64
   ```

4. Configure the build:

   ```powershell
   cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++
   ```

</details>

> **Tip**
> Depending on how Clang is packaged, the compiler may be named `clang++` or
> carry a version suffix such as `clang++-22` or `clang++22`. Use whichever
> name your system installed in the `-DCMAKE_CXX_COMPILER=` argument above.

### 3. Compile

```sh
cmake --build build --config Release
```

The compiled `rux` binary is written to the `Bin/Release` directory (`Bin/Release/rux`, or `Bin\Release\rux.exe` on Windows).

### 4. Verify

Run the binary to confirm it works:

```sh
./Bin/Release/rux help
```

```powershell
.\Bin\Release\rux.exe help
```

To use `rux` from anywhere, copy it to a directory on your `PATH` (for example `/usr/local/bin` on Unix-like systems), or add the `Bin/Release` directory to your `PATH`.

## Community

Here’s how you can get [involved](https://rux-lang.dev/community).

## Contributing

The Rux repository is hosted at [rux-lang/Rux](https://github.com/rux-lang/Rux) on GitHub.

Read the [Contributing guide](.github/CONTRIBUTING.md) to get started.

## License

[MIT](LICENSE)
