# Rux on Windows

This guide covers installing a published x86-64 Rux release and building Rux from source on x86-64 or AArch64 Windows. Return to the [main README](../../README.md) for language documentation and project information.

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

Open a new terminal after installation, then verify the compiler:

```powershell
rux version
```

Use the same installation method again to upgrade. The [Windows installer guide](../../Packaging/Windows/README.md) covers installer behavior, options, and maintenance details.

## Building from Source

Rux currently requires Clang 22.1 or newer, CMake 4.3 or newer, Ninja 1.11 or newer, a recent Git installation, and the Windows SDK and C runtime supplied by Visual Studio.

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
   & "$vs\Common7\Tools\Launch-VsDevShell.ps1" -Arch $arch -HostArch $arch -SkipAutomaticLocation
   ```

5. Clone and build Rux from the initialized environment:

   ```powershell
   git clone https://github.com/rux-lang/Rux.git
   Set-Location Rux
   .\Build.ps1
   ```

The script creates a Release build in `Build\` and writes the compiler to `Bin\rux.exe`.

`Build.ps1` selects `windows-x86_64` or `windows-aarch64` from the native host
architecture. Published Windows ZIP, MSI, PowerShell, and Scoop installations
remain x86-64-only for now.

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
