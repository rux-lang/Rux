<#
.SYNOPSIS
    Packs the Windows toolchain prefix for one target from upstream releases.

.DESCRIPTION
    The Windows peer of PackLinux.sh. Nothing is compiled here: the official
    LLVM release archive and the ccache release are downloaded, verified
    against the checksums pinned in .github/Toolchains.env, and the handful of
    tools Rux uses are written into one relocatable prefix.

    clang++.exe in an upstream archive is a full 140 MB copy of clang.exe
    rather than a symlink, so the prefix ships one real binary and creates the
    other spellings as NTFS hardlinks, which need no administrator rights on
    the same volume; copies are the fallback where the volume refuses. The
    driver mode comes from argv[0].

    .github/Scripts/SetupToolchain.ps1 runs this only when the Actions cache
    held no prefix for the current manifest, and the composite action caches
    the result, so this is the cold path: once per pin bump per target.

.PARAMETER Target
    windows-x86_64 or windows-aarch64.

.PARAMETER Prefix
    Directory to write the prefix into. It is replaced.

.EXAMPLE
    ./.github/Scripts/Toolchain/PackWindows.ps1 -Target windows-x86_64 -Prefix "$env:RUNNER_TEMP/rux-toolchain"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('windows-x86_64', 'windows-aarch64')]
    [string] $Target,

    [Parameter(Mandatory)]
    [string] $Prefix
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
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

function Get-Pin {
    param([Parameter(Mandatory)][hashtable] $Manifest, [Parameter(Mandatory)][string] $Key)

    if (-not $Manifest.ContainsKey($Key) -or -not $Manifest[$Key]) {
        throw "'$manifestPath' declares no $Key"
    }
    return $Manifest[$Key]
}

$manifest = Read-Manifest -Path $manifestPath
$slot = $Target.ToUpperInvariant().Replace('-', '_')
$llvmVersion = Get-Pin $manifest 'LLVM_VERSION'
$llvmMajor = $llvmVersion.Split('.')[0]
$llvmBase = Get-Pin $manifest 'LLVM_BASE_URL'
$llvmAsset = Get-Pin $manifest "LLVM_ASSET_$slot"
$llvmSha = Get-Pin $manifest "SHA256_LLVM_$slot"
$ccacheVersion = Get-Pin $manifest 'CCACHE_VERSION'
$ccacheBase = Get-Pin $manifest 'CCACHE_BASE_URL'
$ccacheAsset = Get-Pin $manifest "CCACHE_ASSET_$slot"
$ccacheSha = Get-Pin $manifest "SHA256_CCACHE_$slot"

# A pin that is never checked is decoration, and a checksum still recorded as
# TBD names an input nobody verified, so both stop the pack before any download.
if ($llvmSha -eq 'TBD') { throw "SHA256_LLVM_$slot is still TBD in Toolchains.env" }
if ($ccacheSha -eq 'TBD') { throw "SHA256_CCACHE_$slot is still TBD in Toolchains.env" }

# Windows' own tar and curl, by path: when this script is launched from a bash
# step, PATH leads with Git's GNU tar, which reads the drive letter in C:\...
# as a remote host name.
$tar = Join-Path $env:SystemRoot 'System32\tar.exe'
if (-not (Test-Path -LiteralPath $tar -PathType Leaf)) { throw "'$tar' was not found" }
$curl = Join-Path $env:SystemRoot 'System32\curl.exe'
if (-not (Test-Path -LiteralPath $curl -PathType Leaf)) { throw "'$curl' was not found" }

$work = Join-Path ($env:RUNNER_TEMP ?? $env:TEMP) "rux-pack-$Target"
Remove-Item -LiteralPath $work, $Prefix -Recurse -Force -ErrorAction SilentlyContinue
foreach ($directory in 'download', 'llvm', 'ccache') {
    New-Item -ItemType Directory -Path (Join-Path $work $directory) -Force | Out-Null
}
foreach ($directory in 'bin', 'lib') {
    New-Item -ItemType Directory -Path (Join-Path $Prefix $directory) -Force | Out-Null
}

function Get-Upstream {
    param([Parameter(Mandatory)][string] $Uri, [Parameter(Mandatory)][string] $Sha256)

    # Stage into .partial and rename only after the checksum matches, so an
    # interrupted download can never be mistaken for a verified one.
    $name = ($Uri -split '/')[-1]
    $destination = Join-Path $work "download/$name"
    & $curl --fail --silent --show-error --location --retry 3 --output "$destination.partial" $Uri
    if ($LASTEXITCODE -ne 0) { throw "could not download '$Uri'" }
    $actual = (Get-FileHash -LiteralPath "$destination.partial" -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $Sha256.ToLowerInvariant()) {
        throw "checksum mismatch for '$name': expected $Sha256, got $actual"
    }
    Move-Item -LiteralPath "$destination.partial" -Destination $destination -Force
    return $destination
}

function New-AliasBinary {
    param([Parameter(Mandatory)][string] $Link, [Parameter(Mandatory)][string] $Original)

    try { New-Item -ItemType HardLink -Path $Link -Value $Original -Force | Out-Null }
    catch { Copy-Item -LiteralPath $Original -Destination $Link -Force }
}

# --- LLVM ---------------------------------------------------------------

$llvmArchive = Get-Upstream -Uri "$llvmBase/$llvmAsset" -Sha256 $llvmSha

# zstd decompresses the archive to a plain tarball first: bsdtar reads zstd
# only when its libarchive was built with it, which the runner images do not
# promise, while zstd itself is on every image.
$tarball = Join-Path $work 'llvm.tar'
if (Get-Command zstd -CommandType Application -ErrorAction SilentlyContinue) {
    & zstd -d -q --rm -o $tarball $llvmArchive
    if ($LASTEXITCODE -ne 0) { throw "could not decompress '$llvmAsset'" }
}
else {
    $tarball = $llvmArchive
}

# The archive holds gigabytes of tools and static libraries nothing here uses,
# so its members are listed once and only the ones Rux needs are extracted:
# the compiler, the two analysis tools, llvm-size, llvm-readobj (which the
# Windows import check in Tests/Unit needs), and the resource directory,
# without which clang-tidy cannot parse a translation unit.
$members = @(& $tar -tf $tarball)
if ($LASTEXITCODE -ne 0 -or $members.Count -eq 0) { throw "could not list '$llvmAsset'" }
$top = [regex]::Escape(($members[0] -split '/')[0])
$wanted = @($members | Where-Object {
        $_ -match "^$top/bin/(clang|clang-format|clang-tidy|llvm-size|llvm-readobj)\.exe$" -or
        $_ -match "^$top/lib/clang/$llvmMajor/include/" -or
        $_ -match "^$top/lib/clang/$llvmMajor/lib/windows/clang_rt\.builtins-[^/]+\.lib$"
    })
if ($wanted.Count -eq 0) { throw "'$llvmAsset' has none of the expected members" }
$selected = Join-Path $work 'selected.txt'
[System.IO.File]::WriteAllLines($selected, [string[]]$wanted)
$llvm = Join-Path $work 'llvm'
& $tar -xf $tarball -C $llvm --strip-components=1 -T $selected
if ($LASTEXITCODE -ne 0) { throw "could not extract '$llvmAsset'" }
Remove-Item -LiteralPath $tarball -Force -ErrorAction SilentlyContinue

foreach ($tool in 'clang', 'clang-format', 'clang-tidy', 'llvm-size', 'llvm-readobj') {
    if (-not (Test-Path -LiteralPath (Join-Path $llvm "bin/$tool.exe") -PathType Leaf)) {
        throw "'$llvmAsset' has no bin/$tool.exe"
    }
}
Copy-Item (Join-Path $llvm 'bin/clang.exe') (Join-Path $Prefix 'bin/clang.exe')
foreach ($alias in 'clang++.exe', "clang++-$llvmMajor.exe") {
    New-AliasBinary -Link (Join-Path $Prefix "bin/$alias") -Original (Join-Path $Prefix 'bin/clang.exe')
}
foreach ($tool in 'clang-format', 'clang-tidy', 'llvm-size') {
    Copy-Item (Join-Path $llvm "bin/$tool.exe") (Join-Path $Prefix "bin/$tool-$llvmMajor.exe")
    New-AliasBinary -Link (Join-Path $Prefix "bin/$tool.exe") `
        -Original (Join-Path $Prefix "bin/$tool-$llvmMajor.exe")
}
Copy-Item (Join-Path $llvm 'bin/llvm-readobj.exe') (Join-Path $Prefix 'bin/llvm-readobj.exe')

# clang finds its resource directory at ../lib/clang/<major> relative to the
# binary. The headers are mandatory; of the runtime libraries only the
# builtins could ever be linked, since Rux uses no sanitizer or profiler.
if (-not (Test-Path -LiteralPath (Join-Path $llvm "lib/clang/$llvmMajor/include") -PathType Container)) {
    throw "'$llvmAsset' has no resource headers"
}
New-Item -ItemType Directory -Path (Join-Path $Prefix 'lib/clang') -Force | Out-Null
Copy-Item (Join-Path $llvm "lib/clang/$llvmMajor") (Join-Path $Prefix "lib/clang/$llvmMajor") -Recurse

# --- ccache -------------------------------------------------------------

$ccacheArchive = Get-Upstream -Uri "$ccacheBase/$ccacheAsset" -Sha256 $ccacheSha
$ccache = Join-Path $work 'ccache'
Expand-Archive -LiteralPath $ccacheArchive -DestinationPath $ccache -Force
$ccacheBinary = Get-ChildItem -LiteralPath $ccache -Recurse -Filter 'ccache.exe' | Select-Object -First 1
if (-not $ccacheBinary) { throw "'$ccacheAsset' has no ccache.exe" }
Copy-Item -LiteralPath $ccacheBinary.FullName -Destination (Join-Path $Prefix 'bin/ccache.exe')

# --- Manifest and proof -------------------------------------------------

@(
    "target=$Target"
    "llvm=$llvmVersion"
    "ccache=$ccacheVersion"
) | Set-Content -LiteralPath (Join-Path $Prefix 'MANIFEST')

foreach ($tool in "clang++-$llvmMajor", "clang-format-$llvmMajor", "clang-tidy-$llvmMajor", 'llvm-readobj', 'ccache') {
    & (Join-Path $Prefix "bin/$tool.exe") --version 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "'$Prefix/bin/$tool.exe' does not run" }
}

Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
$size = [math]::Round((Get-ChildItem -LiteralPath $Prefix -Recurse -File | Measure-Object -Property Length -Sum).Sum / 1MB)
Write-Host "Packed $Target into $Prefix ($size MB)"
