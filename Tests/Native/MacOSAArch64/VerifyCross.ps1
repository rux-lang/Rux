param(
    [Parameter(Mandatory = $true)]
    [string]$Rux
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$scriptDir = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $scriptDir "../../..")).Path
$ruxPath = (Resolve-Path $Rux).Path
$outputDir = Join-Path $repoRoot "Bin/Tests/Native/Release/macOS/AArch64"
$snapshotDir = Join-Path ([System.IO.Path]::GetTempPath()) ("rux-macos-aarch64-cross-" + [guid]::NewGuid())

function Fail([string]$Message) {
    throw "macOS AArch64 cross fixture failure: $Message"
}

function Require-Range([byte[]]$Bytes, [uint64]$Offset, [uint64]$Length, [string]$Description) {
    if ($Offset -gt [uint64]$Bytes.Length -or $Length -gt [uint64]$Bytes.Length - $Offset) {
        Fail "$Description extends beyond the image"
    }
}

function Read-Le32([byte[]]$Bytes, [uint64]$Offset) {
    Require-Range $Bytes $Offset 4 "32-bit field"
    return [BitConverter]::ToUInt32($Bytes, [int]$Offset)
}

function Read-Be32([byte[]]$Bytes, [uint64]$Offset) {
    Require-Range $Bytes $Offset 4 "big-endian 32-bit field"
    return [uint32]([uint64]$Bytes[[int]$Offset] * 0x1000000L +
        [uint64]$Bytes[[int]$Offset + 1] * 0x10000L +
        [uint64]$Bytes[[int]$Offset + 2] * 0x100L +
        [uint64]$Bytes[[int]$Offset + 3])
}

function Require-EqualBytes([byte[]]$Expected, [byte[]]$Actual, [string]$Description) {
    if ($Expected.Length -ne $Actual.Length) {
        Fail "$Description has length $($Actual.Length), expected $($Expected.Length)"
    }
    for ($index = 0; $index -lt $Expected.Length; ++$index) {
        if ($Expected[$index] -ne $Actual[$index]) {
            Fail "$Description differs at byte $index"
        }
    }
}

function Inspect-MachO([string]$Artifact, [uint32]$ExpectedFileType) {
    if (-not (Test-Path -LiteralPath $Artifact -PathType Leaf)) {
        Fail "expected artifact '$Artifact' was not produced"
    }

    [byte[]]$bytes = [System.IO.File]::ReadAllBytes($Artifact)
    Require-Range $bytes 0 32 "Mach-O header"
    if ((Read-Le32 $bytes 0) -ne 4277009103) {
        Fail "'$Artifact' is not a little-endian 64-bit Mach-O image"
    }
    if ((Read-Le32 $bytes 4) -ne 0x0100000C) {
        Fail "'$Artifact' does not declare the ARM64 CPU type"
    }
    if ((Read-Le32 $bytes 12) -ne $ExpectedFileType) {
        Fail "'$Artifact' has the wrong Mach-O file type"
    }

    [uint32]$commandCount = Read-Le32 $bytes 16
    [uint32]$commandBytes = Read-Le32 $bytes 20
    [uint64]$commandEnd = 32 + [uint64]$commandBytes
    Require-Range $bytes 32 $commandBytes "Mach-O load-command area"

    [uint64]$commandOffset = 32
    [uint32]$signatureCount = 0
    [uint32]$signatureOffset = 0
    [uint32]$signatureSize = 0
    for ($index = 0; $index -lt $commandCount; ++$index) {
        if ($commandOffset + 8 -gt $commandEnd) {
            Fail "'$Artifact' has a truncated load-command header"
        }
        [uint32]$command = Read-Le32 $bytes $commandOffset
        [uint32]$commandSize = Read-Le32 $bytes ($commandOffset + 4)
        if ($commandSize -lt 8 -or $commandOffset + $commandSize -gt $commandEnd) {
            Fail "'$Artifact' has an invalid load-command size"
        }
        if ($command -eq 0x1D) {
            if ($commandSize -ne 16) {
                Fail "'$Artifact' has an invalid LC_CODE_SIGNATURE command"
            }
            ++$signatureCount
            $signatureOffset = Read-Le32 $bytes ($commandOffset + 8)
            $signatureSize = Read-Le32 $bytes ($commandOffset + 12)
        }
        $commandOffset += $commandSize
    }
    if ($commandOffset -ne $commandEnd) {
        Fail "'$Artifact' load commands do not fill sizeofcmds"
    }
    if ($signatureCount -ne 1) {
        Fail "'$Artifact' has $signatureCount code-signature commands"
    }
    Require-Range $bytes $signatureOffset $signatureSize "Mach-O code signature"

    if ((Read-Be32 $bytes $signatureOffset) -ne 4208856256) {
        Fail "'$Artifact' does not contain an embedded-signature superblob"
    }
    if ((Read-Be32 $bytes ($signatureOffset + 4)) -ne $signatureSize) {
        Fail "'$Artifact' code-signature sizes disagree"
    }
    if ((Read-Be32 $bytes ($signatureOffset + 8)) -ne 1 -or
        (Read-Be32 $bytes ($signatureOffset + 12)) -ne 0) {
        Fail "'$Artifact' signature does not contain exactly one CodeDirectory slot"
    }

    [uint32]$directoryRelativeOffset = Read-Be32 $bytes ($signatureOffset + 16)
    [uint64]$directoryOffset = [uint64]$signatureOffset + $directoryRelativeOffset
    if ((Read-Be32 $bytes $directoryOffset) -ne 4208856066) {
        Fail "'$Artifact' signature slot is not a CodeDirectory"
    }
    [uint32]$directorySize = Read-Be32 $bytes ($directoryOffset + 4)
    if ($directoryRelativeOffset + [uint64]$directorySize -gt $signatureSize) {
        Fail "'$Artifact' CodeDirectory extends beyond the signature"
    }

    [uint32]$hashOffset = Read-Be32 $bytes ($directoryOffset + 16)
    [uint32]$specialSlotCount = Read-Be32 $bytes ($directoryOffset + 24)
    [uint32]$codeSlotCount = Read-Be32 $bytes ($directoryOffset + 28)
    [uint32]$codeLimit = Read-Be32 $bytes ($directoryOffset + 32)
    [uint32]$expectedSlotCount = [uint32](($codeLimit + 4095L) / 4096L)
    if ($specialSlotCount -ne 0 -or $codeLimit -ne $signatureOffset -or $codeSlotCount -ne $expectedSlotCount) {
        Fail "'$Artifact' CodeDirectory slot counts or code limit are invalid"
    }
    if ($bytes[[int]$directoryOffset + 36] -ne 32 -or
        $bytes[[int]$directoryOffset + 37] -ne 2 -or
        $bytes[[int]$directoryOffset + 39] -ne 12) {
        Fail "'$Artifact' CodeDirectory does not use 4 KiB SHA-256 slots"
    }
    if ([uint64]$hashOffset + [uint64]$codeSlotCount * 32 -gt $directorySize) {
        Fail "'$Artifact' CodeDirectory hashes extend beyond the blob"
    }

    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        for ([uint32]$slot = 0; $slot -lt $codeSlotCount; ++$slot) {
            [int]$pageOffset = [int]$slot * 4096
            [int]$pageLength = [Math]::Min(4096, [int]$codeLimit - $pageOffset)
            [byte[]]$actualHash = $sha256.ComputeHash($bytes, $pageOffset, $pageLength)
            [int]$storedHashOffset = [int]($directoryOffset + $hashOffset + [uint64]$slot * 32)
            [byte[]]$storedHash = $bytes[$storedHashOffset..($storedHashOffset + 31)]
            Require-EqualBytes $storedHash $actualHash "'$Artifact' code-slot $slot hash"
        }
    }
    finally {
        $sha256.Dispose()
    }
}

function Build-Fixtures {
    & $ruxPath --manifest (Join-Path $scriptDir "ExitCode/Fixture.toml") build --release --target macos-aarch64 --quiet
    if ($LASTEXITCODE -ne 0) {
        Fail "failed to build the executable fixture"
    }
    & $ruxPath --manifest (Join-Path $scriptDir "Dylib/Library/Fixture.toml") build --release --target macos-aarch64 --quiet
    if ($LASTEXITCODE -ne 0) {
        Fail "failed to build the shared-library fixture"
    }
}

$artifacts = @(
    @{ Name = "MacOSAArch64ExitCode"; FileType = 2 },
    @{ Name = "libMacOSAArch64Dylib.dylib"; FileType = 6 }
)

try {
    New-Item -ItemType Directory -Path $snapshotDir | Out-Null
    Build-Fixtures
    foreach ($artifact in $artifacts) {
        $path = Join-Path $outputDir $artifact.Name
        Inspect-MachO $path $artifact.FileType
        Copy-Item -LiteralPath $path -Destination (Join-Path $snapshotDir $artifact.Name)
    }

    Build-Fixtures
    foreach ($artifact in $artifacts) {
        $path = Join-Path $outputDir $artifact.Name
        Inspect-MachO $path $artifact.FileType
        [byte[]]$first = [System.IO.File]::ReadAllBytes((Join-Path $snapshotDir $artifact.Name))
        [byte[]]$second = [System.IO.File]::ReadAllBytes($path)
        Require-EqualBytes $first $second "'$($artifact.Name)' deterministic rebuild"
    }
}
finally {
    if (Test-Path -LiteralPath $snapshotDir) {
        Remove-Item -LiteralPath $snapshotDir -Recurse -Force
    }
}

Write-Host "macOS AArch64 cross-build inspection passed"
