param(
    [string]$Rux = (Join-Path $PSScriptRoot "../../../Bin/rux.exe")
)

$ErrorActionPreference = "Stop"

$libraryManifest = Join-Path $PSScriptRoot "Library/Fixture.toml"
$loaderManifest = Join-Path $PSScriptRoot "Loader/Fixture.toml"
& $Rux --manifest $libraryManifest build --release --target windows-aarch64 --quiet
if ($LASTEXITCODE -ne 0) {
    throw "Failed to build the Windows AArch64 DLL fixture"
}
& $Rux --manifest $loaderManifest build --release --target windows-aarch64 --quiet
if ($LASTEXITCODE -ne 0) {
    throw "Failed to build the Windows AArch64 DLL loader fixture"
}

$directory = Join-Path $PSScriptRoot "../../../Bin/Tests/Native/Release/Windows/AArch64"
$library = Join-Path $directory "WindowsAArch64Dll.dll"
$importLibrary = Join-Path $directory "WindowsAArch64Dll.lib"
$loader = Join-Path $directory "WindowsAArch64DllLoader.exe"
foreach ($artifact in @($library, $importLibrary, $loader)) {
    if (!(Test-Path $artifact)) {
        throw "Expected fixture artifact '$artifact' was not produced"
    }
}

& $loader
if ($LASTEXITCODE -ne 0) {
    throw "The Windows AArch64 DLL load/call/unload fixture failed with exit code $LASTEXITCODE"
}
