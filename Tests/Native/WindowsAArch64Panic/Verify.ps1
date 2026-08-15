param(
    [string]$Rux = (Join-Path $PSScriptRoot "../../../Bin/rux.exe")
)

$ErrorActionPreference = "Stop"

$manifest = Join-Path $PSScriptRoot "Fixture.toml"
& $Rux --manifest $manifest build --release --target windows-aarch64 --quiet
if ($LASTEXITCODE -ne 0) {
    throw "Failed to build the Windows AArch64 panic fixture"
}

$executable = Join-Path $PSScriptRoot "../../../Bin/Tests/Native/Release/Windows/AArch64/WindowsAArch64Panic.exe"
$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $executable
$startInfo.UseShellExecute = $false
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true
$startInfo.CreateNoWindow = $true

$process = [System.Diagnostics.Process]::new()
$process.StartInfo = $startInfo
if (-not $process.Start()) {
    throw "Failed to start the Windows AArch64 panic fixture"
}
$stdout = $process.StandardOutput.ReadToEnd()
$stderr = $process.StandardError.ReadToEnd()
$process.WaitForExit()

if ($process.ExitCode -eq 0) {
    throw "The panic returned successfully"
}
if ($stdout.Length -ne 0) {
    throw "The panic unexpectedly wrote to stdout: $stdout"
}

$normalized = [regex]::Replace(
    $stderr,
    "\([^)]*Src[/\\]Main\.rux:[0-9]+:[0-9]+\)",
    "(Src/Main.rux:<line>:<column>)"
)
$expected = "Panic: native Windows ARM64 panic fixture`n" +
    "  at Main (Src/Main.rux:<line>:<column>)`n"
if ($normalized -cne $expected) {
    throw "Unexpected panic stderr:`n$stderr"
}
