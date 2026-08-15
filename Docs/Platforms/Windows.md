# Rux on Windows

This guide covers installing and building Rux on x86-64 or AArch64 Windows. Return to the [main README](../../README.md) for language documentation and project information.

## Installing a Release

Choose one of these per-user installation methods; none requires administrator access:

- Run the PowerShell installer:

  ```powershell
  irm https://rux-lang.dev/install.ps1 | iex
  ```

- Install from the official Scoop bucket:

  ```powershell
  scoop bucket add rux-lang https://github.com/rux-lang/Scoop
  scoop install rux
  ```

- Download `rux-windows.msi` from the [latest GitHub release](https://github.com/rux-lang/Rux/releases/latest) and run it.

These automated installers currently install x86-64 Rux. On AArch64 Windows, download `rux-windows-aarch64.zip` from the latest release and extract `rux.exe` into a directory on `PATH`.

Open a new terminal after installation, then verify the compiler:

```powershell
rux version
```

Use the same installation method again to upgrade. The [Windows installer guide](../../Packaging/Windows/README.md) covers installer behavior, options, and maintenance details.

## Building from Source

Rux currently requires Clang 22.1 or newer, CMake 3.30 or newer, Ninja 1.11 or newer, a recent Git installation, and the Windows SDK and C runtime supplied by Visual Studio.

1. Install Visual Studio or Visual Studio Build Tools with the **Desktop development with C++** workload.

2. Install [Scoop](https://scoop.sh/) from a regular, non-administrator PowerShell window if it is not already available:

   ```powershell
   Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser
   Invoke-RestMethod -Uri https://get.scoop.sh | Invoke-Expression
   ```

3. Install the command-line tools:

   ```powershell
   scoop install git llvm cmake ninja
   ```

4. Use the Native Tools Command Prompt matching the host architecture. To use an existing PowerShell window instead, initialize the development environment once per session:

   ```powershell
   $vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
   $arch = if ([System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture -eq "Arm64") { "arm64" } else { "amd64" }
   cmd /c "`"$vs\VC\Auxiliary\Build\vcvarsall.bat`" $arch && set" | ForEach-Object {
       if ($_ -match "^([^=]+)=(.*)$") { Set-Item -LiteralPath "Env:\$($Matches[1])" -Value $Matches[2] }
   }
   ```

5. Clone and build Rux from the initialized environment:

   ```powershell
   git clone https://github.com/rux-lang/Rux.git
   Set-Location Rux
   .\Build.ps1
   ```

The script creates a Release build in `Build\` and writes the compiler to `Bin\rux.exe`.

Once the repository is cloned, later sessions can replace the snippet in step 4 with the script CI uses, which performs the same initialization for the requested toolset:

```powershell
./.github/scripts/Enter-VsDevEnv.ps1 -Arch amd64   # arm64 on an AArch64 host
```

`Build.ps1` selects `windows-x86_64` or `windows-aarch64` from the native host architecture. Both compilers can emit Windows x86-64 and Classic Windows AArch64 programs. The AArch64 backend and PE/COFF writer produce executables, DLLs with import libraries, and static libraries in-process; no external assembler, compiler, linker, or archiver is invoked.

## Cross-Compiling for Windows AArch64

Pass the canonical target to build or check an AArch64 package from either compiler architecture (`windows-arm64` is accepted as an alias). Target tests are also available when the underlying Windows machine is AArch64:

```powershell
./Bin/rux.exe build --release --target windows-aarch64
./Bin/rux.exe check --target windows-aarch64
./Bin/rux.exe test --release --target windows-aarch64
```

Run workspace commands from the repository root; use `--manifest <path>` before the subcommand for an individual package. Every build is placed below its own operating-system and architecture directory, such as `Bin/Release/Windows/AArch64/Name.exe`, including when that target is the host.

`rux build --all` also produces both Windows targets in Debug and Release; see the [matrix path and flag rules](../Builds.md#building-the-complete-matrix).

`rux run` always builds and launches the compiler's host triple and does not accept `--target`. On AArch64 Windows, both a native AArch64 compiler and an x86-64 compiler process running under Windows translation can launch generated AArch64 tests directly. Rux queries the native OS architecture separately from the compiler process architecture, which is why that x86-64 `rux.exe` can run `rux test --target windows-aarch64`. On physical x86-64 Windows, `build` and `check` still work, but target tests fail before compiling the suite and direct the user to test on an AArch64 machine.

GitHub's `windows-11-arm` runner is the default native and cross-target test environment. An Azure Windows 11 ARM64 VM is reserved for interactive crash dumps, prolonged debugging, or demonstrated GitHub-runner instability; it is not required for acceptance.

## Native Package Artifacts

An `Executable` package writes `Name.exe`, a `SharedLibrary` writes `Name.dll` plus the `Name.lib` import library, and a `StaticLibrary` writes `Name.lib`. Shared and static libraries can be built but not passed to `rux run`. `SourceLibrary` has no standalone native artifact.

The x86-64 and AArch64 backends write PE/COFF objects and libraries directly, without an external toolchain. Executables prefer image base `0x140000000` and DLLs `0x180000000`, and every 64-bit absolute fixup is listed as an `IMAGE_REL_BASED_DIR64` entry in the `.reloc` table, so both PE architectures are relocatable and opt into ASLR with high-entropy addresses. That table is what lets AArch64 images launch at all: Windows on ARM64 refuses an executable or DLL it cannot relocate. ARM64 unwind metadata (`.pdata` / `.xdata`) and broader PE hardening remain follow-up work.

For a Debug build, run `.\Build.ps1 -Configuration Debug`. Run `Get-Help .\Build.ps1 -Full` to see every option.

## Verifying the Build

Run the compiler:

```powershell
.\Bin\rux.exe version
```

Run the complete repository verification workflow:

```powershell
.\Test.ps1
```

Static analysis is intentionally opt-in because it is slower and requires PowerShell 7 or newer:

```powershell
.\Test.ps1 -ClangTidy
```

Use `.\Format.ps1` to format maintained C++ and Rux sources, or `.\Format.ps1 -Check` to check them without making changes.

On AArch64 Windows, reproduce the CI cross-target coverage with an x86-64 or native compiler:

```powershell
.\Bin\rux.exe check --target windows-aarch64
.\Bin\rux.exe test --release --target windows-aarch64
.\Tests\Native\WindowsAArch64ExitCode\Verify.ps1 -Rux .\Bin\rux.exe
.\Tests\Native\WindowsAArch64Dll\Verify.ps1 -Rux .\Bin\rux.exe
```
