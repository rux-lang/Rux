# Build rux with MSVC (requires Visual Studio 2022 Build Tools).
$ErrorActionPreference = "Stop"
$vcvars = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
$cmake = "${env:ProgramFiles}\CMake\bin\cmake.exe"
$root = $PSScriptRoot

cmd /c "`"$vcvars`" && cd /d `"$root`" && `"$cmake`" -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=cl -DCMAKE_BUILD_TYPE=Release && `"$cmake`" --build build"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "Built: $root\build\rux.exe"
