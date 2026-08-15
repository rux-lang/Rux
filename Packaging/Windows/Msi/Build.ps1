#Requires -Version 7.0
<#
.SYNOPSIS
    Build the per-user Rux MSI installer with the WiX v6 .NET tool.

.DESCRIPTION
    Bootstraps the `wix` global tool and the WixToolset.UI extension if they
    are missing, resolves the product version from CMakeLists.txt (unless
    overridden), and produces an .msi in the output directory.

.PARAMETER RuxExe
    Path to the rux.exe to package. Defaults to <repo>\Bin\rux.exe.

.PARAMETER Version
    Product version. Defaults to the VERSION parsed from CMakeLists.txt.

.PARAMETER OutDir
    Directory for the produced .msi. Defaults to .\out next to this script.

.EXAMPLE
    ./Build.ps1
    # Builds rux-windows.msi from Bin\rux.exe.

.EXAMPLE
    ./Build.ps1 -RuxExe Bin\rux.exe -Version 0.3.0
#>
[CmdletBinding()]
param(
    [string]$RuxExe,
    [string]$Version,
    [string]$OutDir = (Join-Path $PSScriptRoot 'out')
)

$ErrorActionPreference = 'Stop'
trap {
    [Console]::Error.WriteLine("error: $($_.Exception.Message)")
    exit 1
}
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$startedAt = Get-Date

function Format-BuildDuration {
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

# --- Resolve the rux.exe to package ---------------------------------------
if (-not $RuxExe) {
    $RuxExe = Join-Path $repoRoot 'Bin\rux.exe'
}
if (-not (Test-Path $RuxExe)) {
    throw "rux.exe was not found at '$RuxExe'; build it first or pass '-RuxExe'"
}
$RuxExe = (Resolve-Path $RuxExe).Path

# --- Resolve the version ---------------------------------------------------
if (-not $Version) {
    $cmake = Get-Content (Join-Path $repoRoot 'CMakeLists.txt') -Raw
    if ($cmake -match 'project\([^)]*VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
        $Version = $Matches[1]
    }
    else {
        throw "version was not found in 'CMakeLists.txt'; pass '-Version'"
    }
}

# --- Ensure the WiX toolchain ---------------------------------------------
# Pin to WiX v6: v7+ gates usage behind the paid Open Source Maintenance Fee
# (OSMF) EULA, which would break unattended/CI builds.
$WixVersion = '6.*'
if (-not (Get-Command wix -ErrorAction SilentlyContinue)) {
    Write-Host "Installing WiX .NET global tool v$WixVersion"
    dotnet tool install --global wix --version $WixVersion
    if ($LASTEXITCODE -ne 0) {
        throw "command 'dotnet tool install' failed with exit code $LASTEXITCODE"
    }
    $env:PATH += ";$env:USERPROFILE\.dotnet\tools"
}

# Register the UI extension, version-matched to the installed wix tool (a
# mismatched extension major version is rejected). Idempotent: re-adding an
# already-registered extension is a no-op.
$wixVer = (wix --version) -replace '\+.*$', ''   # e.g. "6.0.2+hash" -> "6.0.2"
Write-Host "Installing WixToolset.UI extension v$wixVer"
$extensionOutput = wix extension add -g "WixToolset.UI.wixext/$wixVer" 2>&1
if ($LASTEXITCODE -ne 0) {
    throw "command 'wix extension add' failed with exit code ${LASTEXITCODE}: $extensionOutput"
}

# --- Build -----------------------------------------------------------------
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$msi = Join-Path $OutDir 'rux-windows.msi'

Write-Host "Building Rux v$Version (windows-x86_64)"
Write-Host "  Source: '$RuxExe'"
Write-Host "  Output: '$msi'"

# wix resolves relative source paths (LICENSE.md, License.rtf, Readme.txt) against
# the working directory, so run from this script's folder. RuxExe/OutDir were
# already resolved to absolute paths above, so they stay correct.
Push-Location $PSScriptRoot
try {
    wix build 'Rux.wxs' `
        -arch x64 `
        -ext WixToolset.UI.wixext `
        -d Version=$Version `
        -d RuxExe=$RuxExe `
        -o $msi
    if ($LASTEXITCODE -ne 0) {
        throw "command 'wix build' failed with exit code $LASTEXITCODE"
    }
}
finally {
    Pop-Location
}

$elapsed = Format-BuildDuration -Duration ((Get-Date) - $startedAt)
Write-Host "Built Rux MSI in $elapsed" -ForegroundColor Green
Write-Host "  Output: '$msi'"
