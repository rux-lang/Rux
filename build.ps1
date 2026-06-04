# Build rux with CMake on Windows.
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $cmake = Join-Path $env:ProgramFiles "CMake\bin\cmake.exe"
}
if (-not (Test-Path $cmake)) {
    throw "cmake was not found on PATH or under Program Files"
}

$vsGenerator = "Visual Studio 17 2022"
$buildDir = Join-Path $root "build\msvc"

& $cmake -S $root -B $buildDir -G $vsGenerator -A x64
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $cmake --build $buildDir --config Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Built: $buildDir\Release\rux.exe"
