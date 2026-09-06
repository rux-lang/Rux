# Run after tool setup; SDK/runtime changes must not share compilation entries.
[CmdletBinding()]
param([string]$Compiler, [ValidateSet('amd64', 'arm64', '')][string]$VsArch = '')
$ErrorActionPreference = 'Stop'
$parts = @('Release', 'pch-off', $env:ImageOS)
if ($IsWindows) {
    & "$PSScriptRoot/../Install/VsEnvironment.ps1" -Arch $VsArch
    $parts += @($env:VCToolsVersion, $env:WindowsSDKVersion, $env:INCLUDE, $env:LIB)
} elseif ($IsMacOS) {
    $parts += @((& xcrun --show-sdk-version), (& xcodebuild -version))
    if ($LASTEXITCODE -ne 0) { throw 'Could not identify the Apple SDK' }
} else {
    $parts += @((& ldd --version | Select-Object -First 1), (& $Compiler -print-file-name=libstdc++.so))
}
$parts += @(& $Compiler --version)
if ($LASTEXITCODE -ne 0) { throw 'Could not identify the compiler' }
$bytes = [Text.Encoding]::UTF8.GetBytes($parts -join "`n")
$identity = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()
if ($env:GITHUB_OUTPUT) { Add-Content $env:GITHUB_OUTPUT "identity=$identity" }
Write-Host "Compilation environment: $identity"
