# Rux Windows Installer (MSI)

Builds a per-user MSI that installs the Rux compiler on Windows and makes `rux` available from any terminal.

## What it does

- Installs `rux.exe` to `%LocalAppData%\Programs\Rux` (**no admin / UAC prompt**).
- Adds that directory to the **user** `PATH`.
- Bundles `LICENSE` and a short `Readme.txt` alongside the binary.
- Registers an Add/Remove Programs entry, with clean in-place upgrades and a full uninstall (binary, files, and the `PATH` entry are all removed).

The wizard shows the MIT license (EULA) and a choose-install-location page.

## Layout

| File          | Purpose                                                     |
| ------------- | ----------------------------------------------------------- |
| `Rux.wxs`     | WiX v6 authoring (the installer definition).                |
| `License.rtf` | MIT license shown on the EULA page.                         |
| `Readme.txt`  | End-user readme installed alongside the binary.             |
| `Build.ps1`   | Build script: bootstraps WiX, resolves the version, builds. |

## Prerequisites

- **.NET SDK** (provides `dotnet`). The build script installs the [WiX v6](https://wixtoolset.org/) global tool (`wix`) and the `WixToolset.UI` extension automatically on first run.
- A built `rux.exe` (`cmake --build build --config Release`).

## Build locally

From this directory (PowerShell 7+):

```powershell
./Build.ps1
```

This reads the version from `CMakeLists.txt`, packages `..\..\..\Bin\Release\rux.exe`, and writes `out\rux-windows.msi` (the version is embedded in the MSI itself).

Override inputs as needed:

```powershell
./Build.ps1 -RuxExe ..\..\..\Bin\Release\rux.exe -Version 0.3.0 -OutDir out
```

### Building wix directly

```powershell
dotnet tool install --global wix
wix extension add -g WixToolset.UI.wixext
wix build Rux.wxs -arch x64 -ext WixToolset.UI.wixext `
  -d Version=0.3.0 -d RuxExe=..\..\..\Bin\Release\rux.exe -o out\rux-windows.msi
```

## Install / uninstall

```powershell
# Silent install
msiexec /i rux-windows.msi /qn

# Uninstall
msiexec /x rux-windows.msi /qn
```

Double-clicking the `.msi` runs the interactive wizard. After installing, open a **new** terminal so the updated `PATH` is picked up, then run `rux help`.

## CI

The release workflow (`.github/workflows/release.yml`) builds this MSI on the `windows` job and attaches `rux-windows.msi` to the GitHub Release.
