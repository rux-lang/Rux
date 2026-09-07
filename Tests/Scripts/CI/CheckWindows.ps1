# Verify that importing Visual Studio cannot replace the validated build tools,
# in either the setup process or the PATH reconstructed by the Actions runner.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$fixture = Join-Path ([System.IO.Path]::GetTempPath()) ("rux-ci-windows-$([guid]::NewGuid().ToString('N'))")
$originalEnvironment = @{}
Get-ChildItem Env: | ForEach-Object { $originalEnvironment[$_.Name] = $_.Value }
try {
    $scripts = Join-Path $fixture '.github/Scripts'
    $runnerBin = Join-Path $fixture 'runner tools'
    $vsBin = Join-Path $fixture 'VS tools'
    New-Item -ItemType Directory -Path "$scripts/Toolchain", $runnerBin, $vsBin | Out-Null
    Copy-Item "$repositoryRoot/.github/Scripts/SetupToolchain.ps1" $scripts
    Set-Content "$fixture/.github/Toolchains.env" 'CCACHE_MAXSIZE=400M'
    Set-Content "$runnerBin/cmake.cmd" "@echo off`necho cmake version 3.31.0"
    Set-Content "$runnerBin/ninja.cmd" "@echo off`necho 1.13.2"
    Set-Content "$vsBin/cmake.cmd" "@echo off`necho cmake version 3.30.0"
    Set-Content "$vsBin/ninja.cmd" "@echo off`necho 1.12.1"
    $env:RUX_TEST_COMPILER = (Get-Command clang++ -CommandType Application | Select-Object -First 1).Source
    $env:RUX_TEST_VS_BIN = $vsBin
    Set-Content "$scripts/Toolchain/PackWindows.ps1" @'
param($Target, $Prefix)
New-Item -ItemType Directory -Path "$Prefix/bin" -Force | Out-Null
foreach ($tool in 'clang++-23.exe', 'clang-format-23.exe', 'clang-tidy-23.exe', 'llvm-readobj.exe', 'ccache.exe') {
    # Only --version is used, so one real executable stands in for all tools.
    Copy-Item -LiteralPath $env:RUX_TEST_COMPILER -Destination "$Prefix/bin/$tool"
}
'@
    Set-Content "$scripts/VsDevEnv.ps1" @'
param($Arch)
$env:PATH = "$env:RUX_TEST_VS_BIN;$env:PATH"
Add-Content -LiteralPath $env:GITHUB_PATH -Value $env:RUX_TEST_VS_BIN
'@
    $env:GITHUB_ENV = Join-Path $fixture 'github-env'
    $env:GITHUB_PATH = Join-Path $fixture 'github-path'
    $initialPath = "$runnerBin;$env:PATH"
    foreach ($attempt in 1, 2) {
        $env:PATH = $initialPath
        Set-Content -LiteralPath $env:GITHUB_PATH -Value ''
        & "$scripts/SetupToolchain.ps1" -Target windows-aarch64 -Prefix "$fixture/prefix"
        if ((Get-Command ninja -CommandType Application | Select-Object -First 1).Source -ne (Join-Path $runnerBin 'ninja.cmd')) {
            throw 'Visual Studio shadowed Ninja in the setup process'
        }
        # Actions prepends each recorded path, including paths already present.
        $env:PATH = $initialPath
        foreach ($entry in Get-Content -LiteralPath $env:GITHUB_PATH) {
            if ($entry) { $env:PATH = "$entry;$env:PATH" }
        }
        foreach ($tool in 'cmake', 'ninja') {
            $resolved = (Get-Command $tool -CommandType Application | Select-Object -First 1).Source
            if ($resolved -ne (Join-Path $runnerBin "$tool.cmd")) { throw "Visual Studio shadowed $tool in a later step" }
        }
        if ((& ninja --version) -ne '1.13.2') { throw 'The later step ran an outdated Ninja' }
    }
    Write-Host 'Windows CI helper checks passed'
}
finally {
    foreach ($entry in @(Get-ChildItem Env:)) {
        if (-not $originalEnvironment.ContainsKey($entry.Name)) { Remove-Item -LiteralPath "Env:\$($entry.Name)" }
    }
    foreach ($name in $originalEnvironment.Keys) { Set-Item -LiteralPath "Env:\$name" -Value $originalEnvironment[$name] }
    $resolvedFixture = [System.IO.Path]::GetFullPath($fixture)
    $tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
    if (-not $resolvedFixture.StartsWith($tempRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove fixture outside $tempRoot"
    }
    Remove-Item -LiteralPath $resolvedFixture -Recurse -Force
}
