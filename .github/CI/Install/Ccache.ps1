[CmdletBinding()]
param([string]$Destination = (Join-Path $env:RUNNER_TEMP 'rux-build-tools/ccache'))
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/VerifiedDownload.ps1"
$arch = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
$checksums = @{
    X64 = '2568347a697e103ca1b073981c704ad76fb2507d066c38dba038dd73399d968f'
    Arm64 = 'baa07085f68f75ea033d6f8d8fe6b53b7f353050bd191e299ff27e131cc3d787'
}
if (-not $IsWindows -or -not $checksums.ContainsKey($arch)) { throw "Unsupported ccache host: $arch" }
$name = if ($arch -eq 'X64') { 'x86_64' } else { 'aarch64' }
$archive = Join-Path $Destination "ccache-4.14-windows-$name.zip"
$marker = Join-Path $Destination '.identity'
if (-not (Test-Path $marker) -or (Get-Content $marker -Raw).Trim() -ne $checksums[$arch]) {
    Get-VerifiedDownload -Url "https://github.com/ccache/ccache/releases/download/v4.14/ccache-4.14-windows-$name.zip" -Path $archive -Sha256 $checksums[$arch]
    Expand-Archive -LiteralPath $archive -DestinationPath $Destination -Force
    Remove-Item -LiteralPath $archive -Force
}
$binaries = @(Get-ChildItem -LiteralPath $Destination -Filter ccache.exe -Recurse)
if ($binaries.Count -ne 1) { throw 'Expected exactly one ccache executable' }
& $binaries[0].FullName --version
if ($LASTEXITCODE -ne 0) { throw 'ccache installation could not run' }
Set-Content -LiteralPath $marker -Value $checksums[$arch] -Encoding utf8NoBOM
$env:PATH = "$($binaries[0].DirectoryName);$env:PATH"
if ($env:GITHUB_PATH) { Add-Content -LiteralPath $env:GITHUB_PATH -Value $binaries[0].DirectoryName }
