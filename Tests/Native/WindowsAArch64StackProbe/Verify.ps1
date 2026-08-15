param(
    [string]$Rux = (Join-Path $PSScriptRoot "../../../Bin/rux.exe")
)

$ErrorActionPreference = "Stop"

$manifest = Join-Path $PSScriptRoot "Fixture.toml"
& $Rux --manifest $manifest build --release --target windows-aarch64 --quiet
if ($LASTEXITCODE -ne 0) {
    throw "Failed to build the Windows AArch64 stack-probe fixture"
}

$executable = Join-Path $PSScriptRoot "../../../Bin/Tests/Native/Release/Windows/AArch64/WindowsAArch64StackProbe.exe"

& $executable
if ($LASTEXITCODE -ne 0) {
    throw "The Windows AArch64 stack-probe fixture exited with code $LASTEXITCODE"
}
