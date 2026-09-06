# Install the pinned upstream toolchain for either Windows host architecture.
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('x86_64', 'aarch64')]
    [string]$Arch,
    [string]$Destination = 'C:/LLVM'
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/VerifiedDownload.ps1"
$version = '23.1.0'
$checksums = @{
    x86_64 = '5799ebeca6870e9e61d5b4c8bc869ca8490e8db55f22d1388c4545404864d9e3'
    aarch64 = 'caec9387c7925f61de5f53ced0ccf3a9ab9effbe200ea67787f52aa188471cf8'
}
$identity = "$version-$Arch-$($checksums[$Arch])"
$marker = Join-Path $Destination '.rux-llvm-identity'
if ((Test-Path -LiteralPath $marker) -and (Get-Content -LiteralPath $marker -Raw).Trim() -eq $identity -and
    (Test-Path -LiteralPath "$Destination/bin/clang++.exe")) {
    & "$Destination/bin/clang++.exe" --version
    if ($LASTEXITCODE -ne 0) { throw 'Cached LLVM installation could not run clang++' }
    return
}
$archive = Join-Path $env:RUNNER_TEMP "clang+llvm-$version-$Arch-pc-windows-msvc.tar.xz"
$url = "https://github.com/llvm/llvm-project/releases/download/llvmorg-$version/clang+llvm-$version-$Arch-pc-windows-msvc.tar.xz"
Get-VerifiedDownload -Url $url -Path $archive -Sha256 $checksums[$Arch]
New-Item -ItemType Directory -Force -Path $Destination | Out-Null
tar -xf $archive -C $Destination --strip-components=1
if ($LASTEXITCODE -ne 0) { throw 'LLVM archive extraction failed' }
& "$Destination/bin/clang++.exe" --version
if ($LASTEXITCODE -ne 0) { throw 'LLVM installation could not run clang++' }
Set-Content -LiteralPath $marker -Value $identity -Encoding utf8NoBOM
