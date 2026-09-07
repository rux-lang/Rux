<#
.SYNOPSIS
    Prepares the pinned toolchain prefix for one Windows target and exports it.

.DESCRIPTION
    CI installs no toolchain. The composite action restores the prefix from
    the Actions cache; this script packs it from the pinned upstream assets
    only when the cache held nothing for the current manifest, checks the
    CMake and Ninja the runner image ships, and exports the compiler and
    ccache settings every later step uses. This is the Windows peer of
    .github/Scripts/SetupToolchain.sh.

    It also imports the Visual Studio developer environment once and writes it
    to GITHUB_ENV, so every later step in the job inherits the Windows SDK and
    CRT without re-running vcvarsall. vcvarsall.bat is used rather than
    Launch-VsDevShell.ps1 because the latter still rejects the native ARM64
    toolset on windows-11-arm runners.

.PARAMETER Target
    windows-x86_64 or windows-aarch64.

.PARAMETER Prefix
    Directory holding the prefix. Defaults to $env:RUNNER_TEMP\rux-toolchain.

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
$packerPath = Join-Path $PSScriptRoot 'Toolchain/PackWindows.ps1'

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

function Get-ToolVersion {
    param([Parameter(Mandatory)][string] $Name, [Parameter(Mandatory)][string] $Minimum)

    $command = Get-Command $Name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $command) {
        throw "$Name was not found on PATH; the runner image is expected to ship $Name $Minimum or newer"
    }
    $text = (& $command.Source --version 2>&1 | Select-Object -First 1) -replace '^[^0-9]*', '' -replace '[^0-9.].*$', ''
    if ([version]$text -lt [version]$Minimum) {
        throw "the runner image ships $Name $text but Rux requires $Minimum or newer"
    }
    return $text
}

$manifest = Read-Manifest -Path $manifestPath
if (-not $manifest.ContainsKey('CCACHE_MAXSIZE')) { throw "'$manifestPath' declares no CCACHE_MAXSIZE" }
if (-not (Test-Path -LiteralPath $packerPath -PathType Leaf)) { throw "'$packerPath' was not found" }

if (-not $Prefix) {
    $Prefix = Join-Path ($env:RUNNER_TEMP ?? $env:TEMP) 'rux-toolchain'
}

# The marker records which manifest and packer produced the prefix, so a
# restored cache is used only when both still match; the cache key hashes the
# same files, so a mismatch here means a stale prefix, not a stale key.
$sha256 = [System.Security.Cryptography.SHA256]::Create()
$bytes = [System.IO.File]::ReadAllBytes($manifestPath) + [System.IO.File]::ReadAllBytes($packerPath)
$revision = "$Target-" + (($sha256.ComputeHash($bytes) | ForEach-Object { $_.ToString('x2') }) -join '')
$marker = Join-Path $Prefix '.rux-toolchain-revision'
if ((Test-Path -LiteralPath $marker -PathType Leaf) -and
    (Get-Content -LiteralPath $marker -Raw).Trim() -eq $revision) {
    Write-Host "Toolchain $Target is already present in $Prefix"
}
else {
    Write-Host "Packing the $Target toolchain into $Prefix"
    & $packerPath -Target $Target -Prefix $Prefix
    Set-Content -LiteralPath $marker -Value $revision -NoNewline
}

foreach ($tool in 'clang++-23.exe', 'clang-format-23.exe', 'clang-tidy-23.exe', 'llvm-readobj.exe', 'ccache.exe') {
    if (-not (Test-Path -LiteralPath (Join-Path $Prefix "bin/$tool") -PathType Leaf)) {
        throw "'$Prefix/bin/$tool' is missing from the toolchain prefix"
    }
}
$compiler = Join-Path $Prefix 'bin/clang++-23.exe'

# CMake and Ninja come from the runner image, which ships versions inside the
# range CMakeLists.txt accepts; checking here names the real cause when an
# image changes rather than leaving it to the configure step.
$cmakeVersion = Get-ToolVersion -Name cmake -Minimum '3.31'
$ninjaVersion = Get-ToolVersion -Name ninja -Minimum '1.13.2'
$buildToolPaths = @('cmake', 'ninja' | ForEach-Object {
        Split-Path (Get-Command $_ -CommandType Application | Select-Object -First 1).Source
    } | Select-Object -Unique)

# The Visual Studio environment is imported once, here, and exported through
# GITHUB_ENV so no later step has to re-run vcvarsall.
$architecture = if ($Target -eq 'windows-aarch64') { 'arm64' } else { 'amd64' }
& (Join-Path $PSScriptRoot 'VsDevEnv.ps1') -Arch $architecture

# The prefix must win over the Clang vcvarsall and the runner image put on
# PATH. Preserve the checked CMake and Ninja ahead of Visual Studio's bundled
# versions too: vcvarsall can otherwise shadow Ninja with an older release.
# GITHUB_PATH prepends entries in reverse order; CXX uses an absolute path.
$toolchainBin = Join-Path $Prefix 'bin'
$preferredPaths = @($toolchainBin) + $buildToolPaths
if ($env:GITHUB_PATH) {
    for ($index = $preferredPaths.Count - 1; $index -ge 0; $index--) {
        Add-Content -LiteralPath $env:GITHUB_PATH -Value $preferredPaths[$index]
    }
}
$env:PATH = ($preferredPaths -join ';') + ';' + $env:PATH

Add-GitHubEnv -Name 'RUX_TOOLCHAIN' -Value $Prefix
Add-GitHubEnv -Name 'CXX' -Value $compiler
Add-GitHubEnv -Name 'CMAKE_CXX_COMPILER_LAUNCHER' -Value 'ccache'
Add-GitHubEnv -Name 'CCACHE_DIR' -Value (Join-Path ($env:GITHUB_WORKSPACE ?? $repositoryRoot) 'BuildCache/ccache')
Add-GitHubEnv -Name 'CCACHE_MAXSIZE' -Value $manifest['CCACHE_MAXSIZE']
Add-GitHubEnv -Name 'CCACHE_COMPILERCHECK' -Value 'content'
# Other runs build the same tree at another path; without this they would
# never share cache entries.
Add-GitHubEnv -Name 'CCACHE_NOHASHDIR' -Value '1'

& $compiler --version | Select-Object -First 1
& (Join-Path $Prefix 'bin/ccache.exe') --version | Select-Object -First 1
Write-Host "cmake version $cmakeVersion"
Write-Host "ninja $ninjaVersion"
