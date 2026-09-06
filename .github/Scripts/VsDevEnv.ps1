<#
.SYNOPSIS
    Imports the Visual Studio developer environment.

.DESCRIPTION
    Locates the latest Visual Studio installation with vswhere and runs
    vcvarsall.bat for the requested architecture, copying the resulting
    environment variables into this process so Clang, CMake, and the linker find
    the Windows SDK, CRT headers, and import libraries.

    Under GitHub Actions it also writes those variables to GITHUB_ENV and the new
    PATH entries to GITHUB_PATH, so every later step in the job inherits them and
    nothing has to re-run vcvarsall. PATH is deliberately routed to GITHUB_PATH:
    the runner ignores PATH written to GITHUB_ENV.

    vcvarsall.bat is used rather than Launch-VsDevShell.ps1 because the latter
    still restricts its -HostArch parameter to x86 and amd64, which rejects the
    native ARM64 toolset on windows-11-arm runners.

.PARAMETER Arch
    Native toolset to initialize: amd64 (host and target x86-64) or arm64 (host
    and target ARM64). Defaults to the host architecture.

.EXAMPLE
    ./.github/Scripts/VsDevEnv.ps1 -Arch amd64
#>
[CmdletBinding()]
param(
    [ValidateSet('amd64', 'arm64')]
    [string] $Arch
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $Arch) {
    $Arch = switch ([System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture) {
        ([System.Runtime.InteropServices.Architecture]::X64) { 'amd64' }
        ([System.Runtime.InteropServices.Architecture]::Arm64) { 'arm64' }
        default { throw "Windows architecture '$_' is not supported" }
    }
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) { throw "vswhere.exe not found at $vswhere" }

$vsPath = & $vswhere -latest -products * -property installationPath
if (-not $vsPath) { throw 'Visual Studio installation not found' }

$vcvarsall = Join-Path $vsPath 'VC\Auxiliary\Build\vcvarsall.bat'
if (-not (Test-Path -LiteralPath $vcvarsall)) { throw "vcvarsall.bat not found at $vcvarsall" }

$before = @{}
Get-ChildItem Env: | ForEach-Object { $before[$_.Name] = $_.Value }

# Run vcvarsall in a child cmd.exe and dump the environment it produced. The
# commands go through a temporary batch file so no quoting survives the
# PowerShell-to-cmd boundary.
$dumpMarker = '---RUX-VSDEVENV---'
$runner = Join-Path ([System.IO.Path]::GetTempPath()) `
    ("rux-vsdevenv-$([System.Guid]::NewGuid().ToString('N')).cmd")

@"
@echo off
call "$vcvarsall" $Arch || exit /b 1
echo $dumpMarker
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

$markerIndex = [array]::IndexOf([string[]] $output, $dumpMarker)
if ($markerIndex -lt 0) { throw "vcvarsall.bat $Arch produced no environment dump" }

function Add-GitHubEnv {
    param(
        [Parameter(Mandatory)][string] $Name,
        [Parameter(Mandatory)][AllowEmptyString()][string] $Value
    )

    Set-Item -LiteralPath "Env:\$Name" -Value $Value
    if (-not $env:GITHUB_ENV) { return }

    # The heredoc form is the only one that survives a value containing '=',
    # quotes, or a newline, and the random delimiter cannot be forged by a value.
    $delimiter = "RUX$([System.Guid]::NewGuid().ToString('N'))"
    if ($Value -like "*$delimiter*") { throw "refusing to export '$Name'" }
    Add-Content -LiteralPath $env:GITHUB_ENV -Value "$Name<<$delimiter`n$Value`n$delimiter"
}

$existingPath = @($before['PATH'] -split ';' | Where-Object { $_ })
$newPathEntries = [System.Collections.Generic.List[string]]::new()
$imported = 0

foreach ($line in $output[($markerIndex + 1)..($output.Count - 1)]) {
    if ($line -notmatch '^([^=]+)=(.*)$') { continue }
    $name = $Matches[1]
    $value = $Matches[2]

    if ($name -eq 'PATH') {
        foreach ($entry in ($value -split ';')) {
            if ($entry -and $existingPath -notcontains $entry -and $newPathEntries -notcontains $entry) {
                $newPathEntries.Add($entry)
            }
        }
        continue
    }
    # Internal bookkeeping only confuses a second call, and the runner owns the
    # rest.
    if ($name -like '__VSCMD_*' -or $name -in @('PROMPT', 'COMSPEC', 'PATHEXT', 'TEMP', 'TMP')) { continue }
    if ($name -match '^(GITHUB|RUNNER|ACTIONS)_') { continue }
    if ($before.ContainsKey($name) -and $before[$name] -eq $value) { continue }

    Add-GitHubEnv -Name $name -Value $value
    $imported++
}

if ($newPathEntries.Count -gt 0) {
    if ($env:GITHUB_PATH) {
        Add-Content -LiteralPath $env:GITHUB_PATH -Value ($newPathEntries -join [System.Environment]::NewLine)
    }
    $env:PATH = ($newPathEntries -join ';') + ';' + $env:PATH
}

Write-Host "Initialized Visual Studio $Arch environment from $vsPath ($imported variables)"
