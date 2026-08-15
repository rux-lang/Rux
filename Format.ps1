<#
.SYNOPSIS
Formats all maintained C++ and Rux source code in the repository.

.DESCRIPTION
Runs clang-format over Compiler/ and Tests/Unit/ (excluding vendored code),
then runs rux fmt for every package and executable test manifest. Golden
diagnostic fixtures are intentionally excluded because malformed formatting is
part of what they test.

.PARAMETER Check
Checks formatting without modifying files.

.PARAMETER RuxExecutable
An existing rux executable to use. Defaults to Bin/rux or Bin/rux.exe.

.EXAMPLE
./Format.ps1

.EXAMPLE
./Format.ps1 -Check
#>

[CmdletBinding()]
param(
    [switch]$Check,

    [string]$RuxExecutable
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "Scripts/RepositoryMessages.ps1")

function Find-Tool {
    param(
        [Parameter(Mandatory)]
        [string[]]$Name
    )

    foreach ($candidate in $Name) {
        $command = Get-Command $candidate -CommandType Application -ErrorAction SilentlyContinue
        if ($command) {
            return $command.Source
        }
    }

    Stop-Script "required tool '$($Name -join "' or '")' was not found; install it and ensure it is available on PATH"
}

$repositoryRoot = $PSScriptRoot
$clangFormat = Find-Tool -Name @("clang-format-22", "clang-format22", "clang-format")
$runningOnWindows = [System.Environment]::OSVersion.Platform -eq [System.PlatformID]::Win32NT

if ($RuxExecutable) {
    $rux = if ([System.IO.Path]::IsPathRooted($RuxExecutable)) {
        $RuxExecutable
    }
    else {
        Join-Path $repositoryRoot $RuxExecutable
    }
}
else {
    $ruxFileName = if ($runningOnWindows) { "rux.exe" } else { "rux" }
    $rux = Join-Path $repositoryRoot "Bin/$ruxFileName"
}

if (-not (Test-Path -LiteralPath $rux -PathType Leaf)) {
    Stop-Script "rux executable '$rux' was not found; build it first or pass -RuxExecutable"
}

$cppFiles = @(
    Get-ChildItem -LiteralPath (Join-Path $repositoryRoot "Compiler"), (Join-Path $repositoryRoot "Tests/Unit") `
        -File -Recurse |
        Where-Object {
            $_.Extension -in ".cpp", ".h" -and
            $_.FullName -notmatch "[\\/]ThirdParty[\\/]"
        } |
        Sort-Object FullName
)

if ($cppFiles.Count -eq 0) {
    Stop-Script "no maintained C++ files were found"
}

$manifests = @(
    Get-ChildItem -LiteralPath (Join-Path $repositoryRoot "Packages"), (Join-Path $repositoryRoot "Tests") `
        -File -Recurse -Filter "Rux.toml" |
        Sort-Object FullName
)

if ($manifests.Count -eq 0) {
    Stop-Script "no Rux package or test manifests were found"
}

$startedAt = Get-Date
Push-Location $repositoryRoot
try {
    if ($Check) {
        Write-Step "Checking C++ formatting ($($cppFiles.Count) files)"
        foreach ($file in $cppFiles) {
            Invoke-Checked -FilePath $clangFormat -ArgumentList @("--dry-run", "-Werror", $file.FullName)
        }
        Write-Passed "C++ formatting ($($cppFiles.Count) files)"

        Write-Step "Checking Rux formatting ($($manifests.Count) packages)"
        foreach ($manifest in $manifests) {
            Invoke-Checked -FilePath $rux -ArgumentList @("--manifest", $manifest.FullName, "fmt", "--check")
        }
        Write-Passed "Rux formatting ($($manifests.Count) packages)"
    }
    else {
        Write-Step "Formatting C++ sources ($($cppFiles.Count) files)"
        foreach ($file in $cppFiles) {
            Invoke-Checked -FilePath $clangFormat -ArgumentList @("-i", $file.FullName)
        }
        Write-Passed "C++ formatting ($($cppFiles.Count) files)"

        Write-Step "Formatting Rux sources ($($manifests.Count) packages)"
        foreach ($manifest in $manifests) {
            Invoke-Checked -FilePath $rux -ArgumentList @("--manifest", $manifest.FullName, "fmt")
        }
        Write-Passed "Rux formatting ($($manifests.Count) packages)"
    }

    $elapsed = Format-Duration -Duration ((Get-Date) - $startedAt)
    Write-Host ""
    if ($Check) {
        Write-Host "Finished format check in $elapsed" -ForegroundColor Green
    }
    else {
        Write-Host "Finished source formatting in $elapsed" -ForegroundColor Green
    }
}
finally {
    Pop-Location
}
