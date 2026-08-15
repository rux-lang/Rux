#Requires -Version 5.1
<#
.SYNOPSIS
    Install the Rux compiler on Windows for the current user.

.DESCRIPTION
    Downloads the native `rux-windows-<architecture>.zip` asset from a GitHub Release, extracts
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
$startedAt = Get-Date

function Stop-Installer {
    param(
        [Parameter(Mandatory)]
        [string]$Message,

        [string]$Help
    )

    [Console]::Error.WriteLine("error: $Message")
    if ($Help) {
        [Console]::Error.WriteLine("  help: $Help")
    }
    exit 1
}

function Format-InstallerDuration {
    param([Parameter(Mandatory)][TimeSpan]$Duration)

    $milliseconds = [Math]::Max(0, [Math]::Round($Duration.TotalMilliseconds))
    if ($milliseconds -lt 1000) {
        return "$milliseconds ms"
    }
    if ($milliseconds -lt 60000) {
        return ($milliseconds / 1000).ToString("0.## 's'", [Globalization.CultureInfo]::InvariantCulture)
    }

    $minutes = [Math]::Floor($milliseconds / 60000)
    $seconds = ($milliseconds % 60000) / 1000
    return "$minutes min $($seconds.ToString("0.0 's'", [Globalization.CultureInfo]::InvariantCulture))"
}

$architecture = switch ([Runtime.InteropServices.RuntimeInformation]::OSArchitecture) {
    'X64' { 'x86_64' }
    'Arm64' { 'aarch64' }
    default {
        Stop-Installer `
            -Message "architecture '$($_.ToString().ToLowerInvariant())' is not supported by the Windows installer" `
            -Help "build Rux from source: 'https://github.com/$Repo'"
    }
}
$Asset = "rux-windows-$architecture.zip"

# TLS 1.2 for older Windows PowerShell hosts where it isn't the default.
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# --- Resolve the download URL ---------------------------------------------
if ($Version) {
    $Version = $Version.TrimStart('v')
    $tag = "v$Version"
    $url = "https://github.com/$Repo/releases/download/$tag/$Asset"
    Write-Host "Installing Rux v$Version (windows-$architecture)"
}
else {
    # GitHub redirects /releases/latest/download/<asset> to the newest release.
    $url = "https://github.com/$Repo/releases/latest/download/$Asset"
    Write-Host "Installing latest Rux release (windows-$architecture)"
}

# --- Download into a temp folder ------------------------------------------
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("rux-install-" + [Guid]::NewGuid())
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$zip = Join-Path $tmp $Asset

try {
    Write-Host "Downloading '$url'"
    try {
        Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
    }
    catch {
        Stop-Installer `
            -Message "failed to download '$url'" `
            -Help "download the release manually from 'https://github.com/$Repo/releases'"
    }

    Write-Host "Verifying '$Asset'"
    try {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $archive = [IO.Compression.ZipFile]::OpenRead($zip)
        $archive.Dispose()
    }
    catch {
        Stop-Installer `
            -Message "downloaded archive from '$url' is not a valid ZIP archive" `
            -Help 'remove the download and run the installer again'
    }

    # --- Install ----------------------------------------------------------
    try {
        New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    }
    catch {
        Stop-Installer -Message "failed to create install directory '$InstallDir'" -Help 'check that the destination is writable'
    }
    $ruxExe = Join-Path $InstallDir 'rux.exe'
    Write-Host "Installing to '$ruxExe'"
    Write-Host "Extracting '$Asset'"
    try {
        Expand-Archive -Path $zip -DestinationPath $InstallDir -Force
    }
    catch {
        Stop-Installer `
            -Message "failed to extract '$zip' to '$InstallDir'" `
            -Help "extract the archive manually and copy 'rux.exe' to '$InstallDir'"
    }

    if (-not (Test-Path $ruxExe)) {
        Stop-Installer `
            -Message "archive '$Asset' does not contain 'rux.exe'" `
            -Help "download the release manually from 'https://github.com/$Repo/releases'"
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
        try {
            [Environment]::SetEnvironmentVariable('PATH', $newPath, 'User')
        }
        catch {
            Stop-Installer `
                -Message "failed to add '$InstallDir' to the user PATH" `
                -Help "add the directory manually through System Properties > Environment Variables"
        }
        # Reflect it in this session too, so `rux` works without reopening.
        $env:PATH = "$env:PATH;$InstallDir"
        Write-Host "Added '$InstallDir' to the user PATH"
        Write-Host "Restart your terminal to use 'rux' from PATH"
    }
    else {
        Write-Host "PATH already contains '$InstallDir'"
    }
}
elseif (($env:PATH -split ';') -notcontains $InstallDir) {
    Write-Warning "install directory '$InstallDir' is not on PATH"
    Write-Host "  help: add it manually through System Properties > Environment Variables"
}

$elapsed = Format-InstallerDuration -Duration ((Get-Date) - $startedAt)
Write-Host "Installed Rux in $elapsed" -ForegroundColor Green
Write-Host "  Binary: '$ruxExe'"
Write-Host "Run 'rux help' to get started"
