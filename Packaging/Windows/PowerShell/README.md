# Rux PowerShell Installer

`install.ps1` downloads a published `rux-windows.zip`, installs `rux.exe` for the current user, and adds the installation directory to the user PATH. It does not require administrator privileges.

## Quick install

Run from Windows PowerShell 5.1 or PowerShell 7+:

```powershell
irm https://rux-lang.dev/install.ps1 | iex
```

The default destination is `%LocalAppData%\Programs\Rux`. The current session can use `rux` immediately; open a new terminal for the PATH change to appear in other processes.

```powershell
rux version
```

## Version and directory options

Download the script when you need parameters:

```powershell
Invoke-WebRequest `
  https://raw.githubusercontent.com/rux-lang/Rux/main/Packaging/Windows/PowerShell/install.ps1 `
  -OutFile install.ps1

# Pin a version (a leading "v" is also accepted)
.\install.ps1 -Version 0.3.0

# Use a custom directory without changing the user PATH
.\install.ps1 -InstallDir "$HOME\Tools\Rux" -AddToPath:$false
```

| Parameter     | Default                       | Purpose                                                |
| ------------- | ----------------------------- | ------------------------------------------------------ |
| `-Version`    | Latest release                | Install a specific release such as `0.3.0` or `v0.3.0` |
| `-InstallDir` | `%LocalAppData%\Programs\Rux` | Directory that receives `rux.exe`                      |
| `-AddToPath`  | `$true`                       | Add the destination to the current user's PATH         |

Re-running the script replaces `rux.exe` in place. It does not modify or remove the Rux language-package cache at `%LocalAppData%\Rux\Packages`.

## Uninstall

Delete the installation directory, then remove that directory from the user PATH through **System Properties → Environment Variables**. The PowerShell installer does not register an Add/Remove Programs entry; use the [MSI installer](../Msi/README.md) when managed uninstall support is preferred.

## Release integration

The script consumes the `rux-windows.zip` asset created by the [release workflow](../../../.github/workflows/release.yml). Without `-Version`, GitHub's latest-release redirect selects the asset; a pinned version downloads from its matching `v*` tag.
