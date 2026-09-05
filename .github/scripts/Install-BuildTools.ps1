# Install pinned upstream CMake and Ninja archives on native GitHub runners.
# Keep checksums synchronized with the upstream release assets when updating these versions.
[CmdletBinding()]
param(
    [ValidateSet('4.4.3')][string]$CMakeVersion = '4.4.3',
    [ValidateSet('1.13.2')][string]$NinjaVersion = '1.13.2',
    [string]$Destination = (Join-Path $env:RUNNER_TEMP 'rux-build-tools')
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$platform = if ($IsWindows) { 'Windows' } elseif ($IsLinux) { 'Linux' } elseif ($IsMacOS) { 'macOS' } else { throw 'Unsupported runner OS' }
$architecture = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
$archives = @{
    'Windows-X64' = @('cmake-4.4.3-windows-x86_64.zip', '4d52ebab7193a698651639ed80d8d04fd903358843572cf44c7fd234cb7c26ab', 'ninja-win.zip', '07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65')
    'Windows-Arm64' = @('cmake-4.4.3-windows-arm64.zip', '7b410ddd00e24c7250eec7452da2348a4a70437aa87e9cda0a20d6a85662fcff', 'ninja-winarm64.zip', 'e52f0bdef9dfb1003229dbd6508a508c4073fd017247002adc66e5e806cb0391')
    'Linux-X64' = @('cmake-4.4.3-linux-x86_64.tar.gz', 'd6c83076c575bc00b823522ac974bda66d0af05d6ddc30e739c12385cf32c6cc', 'ninja-linux.zip', '5749cbc4e668273514150a80e387a957f933c6ed3f5f11e03fb30955e2bbead6')
    'Linux-Arm64' = @('cmake-4.4.3-linux-aarch64.tar.gz', '2efc974dbd63b4444c0e8494b92f2e80c2d7e635b4b80eac2916985ddd8f72a6', 'ninja-linux-aarch64.zip', 'fd2cacc8050a7f12a16a2e48f9e06fca5c14fc4c2bee2babb67b58be17a607fc')
    'macOS-X64' = @('cmake-4.4.3-macos-universal.tar.gz', '0c5d65251c14cc884bfa16bdbed3c263ce5bffe2e21c0d0d00962cb0610464fa', 'ninja-mac.zip', 'c99048673aa765960a99cf10c6ddb9f1fad506099ff0a0e137ad8960a88f321b')
    'macOS-Arm64' = @('cmake-4.4.3-macos-universal.tar.gz', '0c5d65251c14cc884bfa16bdbed3c263ce5bffe2e21c0d0d00962cb0610464fa', 'ninja-mac.zip', 'c99048673aa765960a99cf10c6ddb9f1fad506099ff0a0e137ad8960a88f321b')
}
$selection = $archives["$platform-$architecture"]
if (-not $selection) { throw "Unsupported runner architecture: $platform-$architecture" }
$Destination = [System.IO.Path]::GetFullPath($Destination)
New-Item -ItemType Directory -Force -Path $Destination | Out-Null

function Get-VerifiedArchive([string]$Url, [string]$Name, [string]$Checksum) {
    $path = Join-Path $Destination $Name
    Invoke-WebRequest -Uri $Url -OutFile $path
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $Checksum) {
        throw "Checksum mismatch for $Name"
    }
    return $path
}
$cmakeArchive = Get-VerifiedArchive "https://github.com/Kitware/CMake/releases/download/v$CMakeVersion/$($selection[0])" $selection[0] $selection[1]
$ninjaArchive = Get-VerifiedArchive "https://github.com/ninja-build/ninja/releases/download/v$NinjaVersion/$($selection[2])" $selection[2] $selection[3]
$cmakeRoot = Join-Path $Destination 'cmake'
$ninjaBin = Join-Path $Destination 'ninja'
New-Item -ItemType Directory -Force -Path $cmakeRoot, $ninjaBin | Out-Null
if ($IsWindows) {
    Expand-Archive -LiteralPath $cmakeArchive -DestinationPath $cmakeRoot -Force
} else {
    tar -xf $cmakeArchive -C $cmakeRoot
    if ($LASTEXITCODE -ne 0) { throw 'CMake archive extraction failed' }
}
Expand-Archive -LiteralPath $ninjaArchive -DestinationPath $ninjaBin -Force
$cmakeDirectory = $selection[0] -replace '\.(zip|tar\.gz)$', ''
$cmakeBin = Join-Path $cmakeRoot "$cmakeDirectory/bin"
if ($IsMacOS) { $cmakeBin = Join-Path $cmakeRoot "$cmakeDirectory/CMake.app/Contents/bin" }
if (-not $IsWindows) {
    chmod +x "$ninjaBin/ninja"
    if ($LASTEXITCODE -ne 0) { throw 'Could not make Ninja executable' }
}
$env:PATH = "$cmakeBin$([System.IO.Path]::PathSeparator)$ninjaBin$([System.IO.Path]::PathSeparator)$env:PATH"
cmake --version
if ($LASTEXITCODE -ne 0) { throw 'CMake installation failed' }
ninja --version
if ($LASTEXITCODE -ne 0) { throw 'Ninja installation failed' }
if ($env:GITHUB_PATH) {
    [System.IO.File]::AppendAllText($env:GITHUB_PATH, "$cmakeBin`n$ninjaBin`n", [System.Text.UTF8Encoding]::new($false))
}
