$ErrorActionPreference = 'Stop'
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$installer = Join-Path $repositoryRoot 'Packaging\Windows\PowerShell\install.ps1'
$fixtureRoot = Join-Path ([IO.Path]::GetTempPath()) ("rux-installer-messages-" + [Guid]::NewGuid())
$archivePath = Join-Path $fixtureRoot 'release.zip'

function Assert-Contains([string]$Text, [string]$Expected) {
    if (-not $Text.Contains($Expected)) {
        throw "installer output does not contain '$Expected':`n$Text"
    }
}

function Invoke-WebRequest {
    [CmdletBinding()]
    param([string]$Uri, [string]$OutFile, [switch]$UseBasicParsing)
    Copy-Item -LiteralPath $archivePath -Destination $OutFile
}

try {
    $payload = Join-Path $fixtureRoot 'payload'
    $installDir = Join-Path $fixtureRoot 'install'
    New-Item -ItemType Directory -Path $payload | Out-Null
    $powershellExecutable = if ($IsWindows) { 'pwsh.exe' } else { 'pwsh' }
    Copy-Item -LiteralPath (Join-Path $PSHOME $powershellExecutable) -Destination (Join-Path $payload 'rux.exe')
    Compress-Archive -Path (Join-Path $payload 'rux.exe') -DestinationPath $archivePath

    $output = (& $installer -Version v1.2.3 -InstallDir $installDir -AddToPath:$false *>&1 | Out-String)
    $architecture = switch ([Runtime.InteropServices.RuntimeInformation]::OSArchitecture) {
        'X64' { 'x86_64' }
        'Arm64' { 'aarch64' }
    }
    Assert-Contains $output "Installing Rux v1.2.3 (windows-$architecture)"
    Assert-Contains $output "/releases/download/v1.2.3/rux-windows-$architecture.zip'"
    Assert-Contains $output "Verifying 'rux-windows-$architecture.zip'"
    Assert-Contains $output "Installing to '$installDir\rux.exe'"
    Assert-Contains $output "install directory '$installDir' is not on PATH"
    Assert-Contains $output 'Installed Rux in '
    Assert-Contains $output "  Binary: '$installDir\rux.exe'"
    if (-not (Test-Path -LiteralPath (Join-Path $installDir 'rux.exe') -PathType Leaf)) {
        throw 'PowerShell installer did not install rux.exe'
    }

    $latestOutput = (& $installer -InstallDir $installDir -AddToPath:$false *>&1 | Out-String)
    Assert-Contains $latestOutput "Installing latest Rux release (windows-$architecture)"
}
finally {
    Remove-Item -LiteralPath $fixtureRoot -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host 'PowerShell installer message tests passed.'
