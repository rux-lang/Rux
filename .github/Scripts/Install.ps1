# CI installation entry point. Implementations live under ci/Install.
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('BuildTools', 'WindowsLLVM', 'Ccache', 'MacOSLLVM', 'LinuxLLVM', 'VsEnvironment')]
    [string]$Tool,
    [string]$Arch,
    [string]$Destination
)
$ErrorActionPreference = 'Stop'
$implementation = Join-Path $PSScriptRoot '../CI/Install'
$parameters = @{}
if ($Destination) { $parameters.Destination = $Destination }
switch ($Tool) {
    'BuildTools' { & "$implementation/BuildTools.ps1" @parameters }
    'WindowsLLVM' {
        $parameters.Arch = $Arch
        & "$implementation/WindowsLLVM.ps1" @parameters
    }
    'Ccache' { & "$implementation/Ccache.ps1" @parameters }
    'VsEnvironment' { & "$implementation/VsEnvironment.ps1" -Arch $Arch }
    'MacOSLLVM' {
        if (-not $Destination) { $Destination = Join-Path $env:RUNNER_TEMP 'rux-macos-tools' }
        & sh "$implementation/MacOS.sh" $Destination
        if ($LASTEXITCODE -ne 0) { throw 'macOS toolchain setup failed' }
    }
    'LinuxLLVM' {
        if (-not $Destination) { $Destination = Join-Path $env:RUNNER_TEMP 'rux-linux-tools' }
        & sh "$implementation/Linux.sh" $Destination
        if ($LASTEXITCODE -ne 0) { throw 'Linux toolchain setup failed' }
    }
}
