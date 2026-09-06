<#
.SYNOPSIS
    Restores the pinned prebuilt toolchain for one Windows target.

.DESCRIPTION
    CI never installs or compiles a toolchain: it downloads exactly one verified
    bundle from rux-lang/Toolchain and puts it on PATH. This is the Windows
    peer of .github/Scripts/SetupToolchain.sh.

    It also imports the Visual Studio developer environment once and writes it to
    GITHUB_ENV, so every later step in the job inherits the Windows SDK and CRT
    without re-running vcvarsall. vcvarsall.bat is used rather than
    Launch-VsDevShell.ps1 because the latter still rejects the native ARM64
    toolset on windows-11-arm runners.

.PARAMETER Target
    windows-x86_64 or windows-aarch64.

.PARAMETER Prefix
    Directory to install the bundle into. Defaults to $env:RUNNER_TEMP\rux-toolchain.

.EXAMPLE
    ./.github/Scripts/SetupToolchain.ps1 -Target windows-x86_64
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('windows-x86_64', 'windows-aarch64')]
    [string] $Target,

    [string] $Prefix
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$manifestPath = Join-Path $repositoryRoot '.github/Toolchains.env'

function Read-Manifest {
    param([Parameter(Mandatory)][string] $Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "'$Path' was not found"
    }

    $values = @{}
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ($line -match '^\s*(#.*)?$') { continue }
        if ($line -notmatch '^([A-Z0-9_]+)=([A-Za-z0-9._:/+-]*)$') {
            throw "'$Path' contains a line that is not a comment or KEY=VALUE: $line"
        }
        $values[$Matches[1]] = $Matches[2]
    }
    return $values
}

function Get-VerifiedDownload {
    param(
        [Parameter(Mandatory)][string] $Uri,
        [Parameter(Mandatory)][string] $Destination,
        [Parameter(Mandatory)][string] $Sha256
    )

    # Stage into .partial and rename only after the checksum matches, so an
    # interrupted download can never be mistaken for a verified one.
    $partial = "$Destination.partial"
    for ($attempt = 1; $attempt -le 3; $attempt++) {
        Remove-Item -LiteralPath $partial -Force -ErrorAction SilentlyContinue
        try {
            Invoke-WebRequest -Uri $Uri -OutFile $partial -UseBasicParsing
            break
        }
        catch {
            if ($attempt -eq 3) { throw "could not download '$Uri': $_" }
            Write-Host "warning: download attempt $attempt of 3 failed for $Uri"
            Start-Sleep -Seconds ($attempt * 5)
        }
    }

    $actual = (Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $Sha256.ToLowerInvariant()) {
        Remove-Item -LiteralPath $partial -Force -ErrorAction SilentlyContinue
        throw "checksum mismatch for '$Uri': expected $Sha256, got $actual"
    }
    Move-Item -LiteralPath $partial -Destination $Destination -Force
}

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

$manifest = Read-Manifest -Path $manifestPath

$revision = $manifest['TOOLCHAIN_REVISION']
if ($revision -eq 'TBD') { throw 'TOOLCHAIN_REVISION is still TBD' }

$checksumName = 'SHA256_TOOLCHAIN_' + $Target.ToUpperInvariant().Replace('-', '_')
if (-not $manifest.ContainsKey($checksumName)) {
    throw "'$manifestPath' declares no $checksumName"
}
$checksum = $manifest[$checksumName]
if ($checksum -eq 'TBD') {
    throw "$checksumName is still TBD; publish a Toolchain release and record its checksum"
}

if (-not $Prefix) {
    $Prefix = Join-Path ($env:RUNNER_TEMP ?? $env:TEMP) 'rux-toolchain'
}

$archiveName = "rux-toolchain-$Target-$revision.zip"
$archiveUrl = "$($manifest['TOOLCHAIN_BASE_URL'])/toolchain-$revision/$archiveName"

# The marker records which revision the prefix already holds, so a warm runner
# cache skips download and extraction entirely.
$marker = Join-Path $Prefix '.rux-toolchain-revision'
if ((Test-Path -LiteralPath $marker -PathType Leaf) -and
    (Get-Content -LiteralPath $marker -Raw).Trim() -eq "$Target-$revision") {
    Write-Host "Toolchain $Target $revision is already present in $Prefix"
}
else {
    $staging = Join-Path ($env:RUNNER_TEMP ?? $env:TEMP) 'rux-toolchain-download'
    New-Item -ItemType Directory -Path $staging -Force | Out-Null
    $archive = Join-Path $staging $archiveName

    Write-Host "Downloading $archiveUrl"
    Get-VerifiedDownload -Uri $archiveUrl -Destination $archive -Sha256 $checksum

    Remove-Item -LiteralPath $Prefix -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Path $Prefix -Force | Out-Null
    # The bundle has a single top-level directory; flatten it into the prefix.
    $unpacked = Join-Path $staging 'unpacked'
    Remove-Item -LiteralPath $unpacked -Recurse -Force -ErrorAction SilentlyContinue
    Expand-Archive -LiteralPath $archive -DestinationPath $unpacked -Force
    $root = @(Get-ChildItem -LiteralPath $unpacked -Directory)
    if ($root.Count -ne 1) { throw "expected one top-level directory in '$archiveName'" }
    Get-ChildItem -LiteralPath $root[0].FullName -Force |
        Move-Item -Destination $Prefix -Force
    Remove-Item -LiteralPath $archive, $unpacked -Recurse -Force -ErrorAction SilentlyContinue
    Set-Content -LiteralPath $marker -Value "$Target-$revision" -NoNewline
}

$compiler = Join-Path $Prefix 'bin/clang++-23.exe'
if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) {
    throw "'$compiler' is missing from the bundle"
}

# The Visual Studio environment is imported once, here, and exported through
# GITHUB_ENV so no later step has to re-run vcvarsall.
$architecture = if ($Target -eq 'windows-aarch64') { 'arm64' } else { 'amd64' }
& (Join-Path $PSScriptRoot 'VsDevEnv.ps1') -Arch $architecture

# The bundle must win over the Clang vcvarsall puts on PATH. GITHUB_PATH
# prepends each write, so this one lands ahead of the entries VsDevEnv.ps1 just
# added; CXX names the compiler by absolute path regardless.
$toolchainBin = Join-Path $Prefix 'bin'
if ($env:GITHUB_PATH) { Add-Content -LiteralPath $env:GITHUB_PATH -Value $toolchainBin }
$env:PATH = "$toolchainBin;$env:PATH"

Add-GitHubEnv -Name 'RUX_TOOLCHAIN' -Value $Prefix
Add-GitHubEnv -Name 'CXX' -Value $compiler
Add-GitHubEnv -Name 'CMAKE_CXX_COMPILER_LAUNCHER' -Value 'ccache'
Add-GitHubEnv -Name 'CCACHE_DIR' -Value (Join-Path ($env:GITHUB_WORKSPACE ?? $repositoryRoot) 'BuildCache/ccache')
Add-GitHubEnv -Name 'CCACHE_MAXSIZE' -Value $manifest['CCACHE_MAXSIZE']
Add-GitHubEnv -Name 'CCACHE_COMPILERCHECK' -Value 'content'
# The FreeBSD guest builds under /root/rux while a host builds under the
# workspace path; without this they would never share cache entries.
Add-GitHubEnv -Name 'CCACHE_NOHASHDIR' -Value '1'

& $compiler --version | Select-Object -First 1
& (Join-Path $Prefix 'bin/cmake.exe') --version | Select-Object -First 1
& (Join-Path $Prefix 'bin/ninja.exe') --version
