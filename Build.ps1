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
. (Join-Path $PSScriptRoot "Scripts/RepositoryMessages.ps1")

function Find-Tool {
    param(
        [Parameter(Mandatory)]
        [string]$Name
    )

    $command = Get-Command $Name -CommandType Application -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    Stop-Script "required tool '$Name' was not found; install it and ensure it is available on PATH"
}

$repositoryRoot = $PSScriptRoot
$buildPath = if ([System.IO.Path]::IsPathRooted($BuildDirectory)) {
    $BuildDirectory
}
else {
    Join-Path $repositoryRoot $BuildDirectory
}

$cmake = Find-Tool -Name "cmake"
$null = Find-Tool -Name "ninja"
$compiler = Find-Tool -Name "clang++"
$compilerTarget = switch ([System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture) {
    ([System.Runtime.InteropServices.Architecture]::X64) { "x86_64-pc-windows-msvc" }
    ([System.Runtime.InteropServices.Architecture]::Arm64) { "aarch64-pc-windows-msvc" }
    default { Stop-Script "Windows architecture '$([System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture)' is not supported" }
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
        "-DCMAKE_CXX_COMPILER=$compiler",
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
        Stop-Script "build completed without producing the expected compiler at '$rux'"
    }

    $elapsed = Format-Duration -Duration ((Get-Date) - $startedAt)
    Write-Host ""
    Write-Host "Finished build in $elapsed" -ForegroundColor Green
    Write-Host "  Output: '$rux'"
}
finally {
    Pop-Location
}
