param(
    [string]$Rux = (Join-Path $PSScriptRoot "../../../Bin/rux.exe")
)

$ErrorActionPreference = "Stop"

$manifest = Join-Path $PSScriptRoot "Fixture.toml"
& $Rux --manifest $manifest build --release --target windows-aarch64 --quiet
if ($LASTEXITCODE -ne 0) {
    throw "Failed to build the Windows AArch64 exit-code fixture"
}

$nativeExecutable = Join-Path $PSScriptRoot "../../../Bin/Tests/Native/Release/WindowsAArch64ExitCode.exe"
$crossExecutable = Join-Path $PSScriptRoot "../../../Bin/Tests/Native/Release/Windows/AArch64/WindowsAArch64ExitCode.exe"
$executable = if (Test-Path $nativeExecutable) { $nativeExecutable } else { $crossExecutable }

& $executable
if ($LASTEXITCODE -ne 73) {
    throw "The Windows AArch64 exit-code fixture returned $LASTEXITCODE instead of 73"
}
