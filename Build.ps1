<#
.SYNOPSIS
Configures and builds the Rux compiler and C++ unit-test target.

.PARAMETER Configuration
The CMake configuration to build. Defaults to Release.

.PARAMETER BuildDirectory
The CMake build directory, relative to the repository root unless absolute.

.EXAMPLE
./Build.ps1

.EXAMPLE
./Build.ps1 -Configuration Debug
#>

[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",

    [string]$BuildDirectory = "Build"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Find-Tool {
    param(
        [Parameter(Mandatory)]
        [string]$Name
    )

    $command = Get-Command $Name -CommandType Application -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    throw "Required tool not found: $Name"
}

function Invoke-Checked {
    param(
        [Parameter(Mandatory)]
        [string]$FilePath,

        [Parameter()]
        [string[]]$ArgumentList = @()
    )

    & $FilePath @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $FilePath $($ArgumentList -join ' ')"
    }
}

function Write-Step {
    param(
        [Parameter(Mandatory)]
        [string]$Message
    )

    Write-Host ""
    Write-Host "==> $Message" -ForegroundColor Cyan
}

$repositoryRoot = $PSScriptRoot
$buildPath = if ([System.IO.Path]::IsPathRooted($BuildDirectory)) {
    $BuildDirectory
}
else {
    Join-Path $repositoryRoot $BuildDirectory
}

$cmake = Find-Tool -Name "cmake"
$compilerTarget = switch ([System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture) {
    ([System.Runtime.InteropServices.Architecture]::X64) { "x86_64-pc-windows-msvc" }
    ([System.Runtime.InteropServices.Architecture]::Arm64) { "aarch64-pc-windows-msvc" }
    default { throw "Unsupported Windows architecture: $([System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture)" }
}
$startedAt = Get-Date

Push-Location $repositoryRoot
try {
    Write-Step "Configuring $Configuration build"
    Invoke-Checked -FilePath $cmake -ArgumentList @(
        "-S", $repositoryRoot,
        "-B", $buildPath,
        "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=$Configuration",
        "-DCMAKE_CXX_COMPILER=clang++",
        "-DCMAKE_CXX_COMPILER_TARGET=$compilerTarget",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        "-DRUX_WERROR=ON",
        "-DRUX_BUILD_TESTS=ON"
    )

    Write-Step "Building compiler and unit tests"
    Invoke-Checked -FilePath $cmake -ArgumentList @("--build", $buildPath, "--config", $Configuration)

    $runningOnWindows = [System.Environment]::OSVersion.Platform -eq [System.PlatformID]::Win32NT
    $ruxFileName = if ($runningOnWindows) { "rux.exe" } else { "rux" }
    $rux = Join-Path $repositoryRoot "Bin/$ruxFileName"
    if (-not (Test-Path -LiteralPath $rux -PathType Leaf)) {
        throw "Build completed without producing the expected compiler at '$rux'."
    }

    $elapsed = (Get-Date) - $startedAt
    Write-Host ""
    Write-Host ("Build passed in {0:mm\:ss}: {1}" -f $elapsed, $rux) -ForegroundColor Green
}
finally {
    Pop-Location
}
