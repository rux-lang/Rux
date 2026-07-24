<#
.SYNOPSIS
    Imports the Visual Studio developer environment into the current PowerShell process.

.DESCRIPTION
    Locates the latest Visual Studio installation with vswhere and runs
    vcvarsall.bat for the requested architecture, copying the resulting
    environment variables into this process so Clang, CMake, and the linker
    find the Windows SDK, CRT headers, and import libraries.

    vcvarsall.bat is used instead of Launch-VsDevShell.ps1 because the latter
    still restricts its -HostArch parameter to x86 and amd64, which rejects the
    native ARM64 toolset on windows-11-arm runners.

.PARAMETER Arch
    Native toolset to initialize: amd64 (host and target x86-64) or arm64
    (host and target ARM64).

.EXAMPLE
    ./.github/scripts/Enter-VsDevEnv.ps1 -Arch arm64
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('amd64', 'arm64')]
    [string] $Arch
)

$ErrorActionPreference = 'Stop'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "vswhere.exe not found at $vswhere"
}

$vsPath = & $vswhere -latest -products * -property installationPath
if (-not $vsPath) {
    throw "Visual Studio installation not found"
}

$vcvarsall = Join-Path $vsPath 'VC\Auxiliary\Build\vcvarsall.bat'
if (-not (Test-Path -LiteralPath $vcvarsall)) {
    throw "vcvarsall.bat not found at $vcvarsall"
}

# Run vcvarsall in a child cmd.exe and dump the environment it produced. The
# commands go through a temporary batch file so no quoting survives the
# PowerShell-to-cmd boundary.
$marker = '---RUX-VSDEVENV---'
$runner = Join-Path ([System.IO.Path]::GetTempPath()) ("rux-vsdevenv-$([System.Guid]::NewGuid().ToString('N')).cmd")

@"
@echo off
call "$vcvarsall" $Arch || exit /b 1
echo $marker
set
"@ | Set-Content -LiteralPath $runner -Encoding ascii

try {
    $output = & "${env:COMSPEC}" /c $runner
    if ($LASTEXITCODE -ne 0) {
        throw "vcvarsall.bat $Arch failed with exit code ${LASTEXITCODE}:`n$($output -join [System.Environment]::NewLine)"
    }
}
finally {
    Remove-Item -LiteralPath $runner -Force -ErrorAction SilentlyContinue
}

$markerIndex = [array]::IndexOf([string[]] $output, $marker)
if ($markerIndex -lt 0) {
    throw "vcvarsall.bat $Arch produced no environment dump:`n$($output -join [System.Environment]::NewLine)"
}

$imported = 0
foreach ($line in $output[($markerIndex + 1)..($output.Count - 1)]) {
    if ($line -match '^([^=]+)=(.*)$') {
        Set-Item -LiteralPath "Env:\$($Matches[1])" -Value $Matches[2]
        $imported++
    }
}

Write-Host "Initialized Visual Studio $Arch environment from $vsPath ($imported variables)"
