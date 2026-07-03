#Requires -Version 5.1
<#
.SYNOPSIS
    Install the Rux compiler on Windows for the current user.

.DESCRIPTION
    Downloads the `rux-windows.zip` asset from a GitHub Release, extracts
    `rux.exe` into %LocalAppData%\Programs\Rux, and adds that directory to the
    user PATH. Per-user only: no admin rights or UAC prompt are required.

    Designed to be run directly from the web:

        irm https://rux-lang.dev/install.ps1 | iex

    Re-running upgrades an existing install in place.

.PARAMETER Version
    Release version to install, e.g. "0.3.0" (with or without a leading "v").
    Defaults to the latest published release.

.PARAMETER InstallDir
    Target directory. Defaults to %LocalAppData%\Programs\Rux.

.PARAMETER AddToPath
    Add the install directory to the user PATH. Defaults to $true; pass
    -AddToPath:$false to skip the PATH update.

.EXAMPLE
    irm https://rux-lang.dev/install.ps1 | iex

.EXAMPLE
    .\install.ps1 -Version 0.3.0
#>
[CmdletBinding()]
param(
    [string]$Version,
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'Programs\Rux'),
    [bool]$AddToPath = $true
)

$ErrorActionPreference = 'Stop'
$Repo = 'rux-lang/Rux'
$Asset = 'rux-windows.zip'

# TLS 1.2 for older Windows PowerShell hosts where it isn't the default.
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# --- Resolve the download URL ---------------------------------------------
if ($Version) {
    $tag = if ($Version.StartsWith('v')) { $Version } else { "v$Version" }
    $url = "https://github.com/$Repo/releases/download/$tag/$Asset"
}
else {
    # GitHub redirects /releases/latest/download/<asset> to the newest release.
    $url = "https://github.com/$Repo/releases/latest/download/$Asset"
}

# --- Download into a temp folder ------------------------------------------
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("rux-install-" + [Guid]::NewGuid())
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$zip = Join-Path $tmp $Asset

try {
    Write-Host "Downloading $url"
    Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing

    # --- Install ----------------------------------------------------------
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    Write-Host "Installing to $InstallDir"
    Expand-Archive -Path $zip -DestinationPath $InstallDir -Force

    $ruxExe = Join-Path $InstallDir 'rux.exe'
    if (-not (Test-Path $ruxExe)) {
        throw "Expected rux.exe in $Asset but it was not found after extraction."
    }
}
finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

# --- Add to the user PATH (idempotent) ------------------------------------
if ($AddToPath) {
    $userPath = [Environment]::GetEnvironmentVariable('PATH', 'User')
    $parts = if ($userPath) { $userPath -split ';' } else { @() }
    if ($parts -notcontains $InstallDir) {
        $newPath = (@($userPath, $InstallDir) | Where-Object { $_ }) -join ';'
        [Environment]::SetEnvironmentVariable('PATH', $newPath, 'User')
        # Reflect it in this session too, so `rux` works without reopening.
        $env:PATH = "$env:PATH;$InstallDir"
        Write-Host "Added $InstallDir to your user PATH."
        Write-Host "Open a NEW terminal for the PATH change to take effect everywhere."
    }
}

$installed = (& (Join-Path $InstallDir 'rux.exe') --version 2>$null) -join ' '
Write-Host ""
Write-Host "Rux installed successfully. $installed"
Write-Host "Run 'rux help' to get started."
