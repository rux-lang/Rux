<#
.SYNOPSIS
Builds and runs the complete repository verification workflow.

.DESCRIPTION
The script checks platform isolation, invokes Build.ps1, verifies C++ and Rux
formatting through Format.ps1, runs CTest, checks and lints the local Rux
workspace, and runs all Rux test packages. Clang-tidy is optional through
-ClangTidy because a full static-analysis pass is comparatively slow.
It does not rewrite source files unless -FixFormatting is specified.

.PARAMETER Configuration
The CMake configuration to build and test. Defaults to Release.

.PARAMETER BuildDirectory
The CMake build directory, relative to the repository root unless absolute.

.PARAMETER RuxExecutable
An existing rux executable to use. By default, the script uses Bin/rux or
Bin/rux.exe.

.PARAMETER SkipBuild
Skips Build.ps1. The existing CMake test target and rux executable are used.

.PARAMETER FixFormatting
Formats C++ and Rux sources in place instead of only checking their formatting.

.PARAMETER ClangTidy
Runs clang-tidy over every maintained C++ translation unit. This option
requires PowerShell 7 because it analyzes files in parallel.

.EXAMPLE
./Test.ps1

.EXAMPLE
./Test.ps1 -SkipBuild

.EXAMPLE
./Test.ps1 -SkipBuild -ClangTidy
#>

[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",

    [string]$BuildDirectory = "Build",

    [string]$RuxExecutable,

    [switch]$SkipBuild,

    [switch]$FixFormatting,

    [switch]$ClangTidy
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

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

    throw "Required tool not found: $($Name -join ' or ')"
}

function Invoke-Checked {
    param(
        [Parameter(Mandatory)]
        [string]$FilePath,

        [Parameter()]
        [string[]]$ArgumentList = @()
    )

    & $FilePath @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $FilePath $($ArgumentList -join ' ')"
    }
}

function Write-Step {
    param(
        [Parameter(Mandatory)]
        [string]$Message
    )

    Write-Host ""
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Get-ManifestlessCheckScopes {
    param(
        [Parameter(Mandatory)]
        [string]$Root
    )

    $manifests = [System.Collections.Generic.List[string]]::new()

    $rootTests = Join-Path $Root "Tests"
    if (Test-Path -LiteralPath $rootTests -PathType Container) {
        Get-ChildItem -LiteralPath $rootTests -Filter "Rux.toml" -File -Recurse |
            ForEach-Object { $manifests.Add($_.FullName) }
    }

    Get-ChildItem -LiteralPath $Root -Directory | ForEach-Object {
        $memberManifest = Join-Path $_.FullName "Rux.toml"
        if (Test-Path -LiteralPath $memberManifest -PathType Leaf) {
            $manifests.Add($memberManifest)
        }

        $memberTests = Join-Path $_.FullName "Tests"
        if (Test-Path -LiteralPath $memberTests -PathType Container) {
            Get-ChildItem -LiteralPath $memberTests -Filter "Rux.toml" -File -Recurse |
                ForEach-Object { $manifests.Add($_.FullName) }
        }
    }

    return @($manifests | Sort-Object -Unique)
}

$repositoryRoot = $PSScriptRoot
$buildPath = if ([System.IO.Path]::IsPathRooted($BuildDirectory)) {
    $BuildDirectory
}
else {
    Join-Path $repositoryRoot $BuildDirectory
}

$ctest = Find-Tool -Name "ctest"
$shell = Find-Tool -Name "sh"
if ($ClangTidy -and $PSVersionTable.PSVersion.Major -lt 7) {
    throw "-ClangTidy requires PowerShell 7 or newer."
}
$clangTidyExecutable = if ($ClangTidy) {
    Find-Tool -Name @("clang-tidy-22", "clang-tidy")
}

$startedAt = Get-Date
Push-Location $repositoryRoot
try {
    Write-Step "Checking platform isolation"
    # A login shell ensures Git for Windows adds dirname and grep to PATH.
    Invoke-Checked -FilePath $shell -ArgumentList @("-lc", "sh Tests/Policy/PlatformIsolation/Check.sh")

    if (-not $SkipBuild) {
        & (Join-Path $repositoryRoot "Build.ps1") `
            -Configuration $Configuration `
            -BuildDirectory $BuildDirectory
    }

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
        throw "Rux executable not found at '$rux'. Build it first or pass -RuxExecutable."
    }

    $formatParameters = @{
        RuxExecutable = $rux
    }
    if (-not $FixFormatting) {
        $formatParameters.Check = $true
    }
    & (Join-Path $repositoryRoot "Format.ps1") @formatParameters

    if ($ClangTidy) {
        $compileCommands = Join-Path $buildPath "compile_commands.json"
        if (-not (Test-Path -LiteralPath $compileCommands -PathType Leaf)) {
            throw "Compilation database not found at '$compileCommands'. Run Test.ps1 without -SkipBuild first."
        }

        $compilerRoot = (Join-Path $repositoryRoot "Compiler") + [System.IO.Path]::DirectorySeparatorChar
        $unitTestRoot = (Join-Path $repositoryRoot "Tests/Unit") + [System.IO.Path]::DirectorySeparatorChar
        $clangTidySources = @(
            Get-Content -LiteralPath $compileCommands -Raw |
                ConvertFrom-Json |
                ForEach-Object { [System.IO.Path]::GetFullPath($_.file) } |
                Where-Object {
                    ($_.StartsWith($compilerRoot, [System.StringComparison]::OrdinalIgnoreCase) -or
                        $_.StartsWith($unitTestRoot, [System.StringComparison]::OrdinalIgnoreCase)) -and
                    $_ -notmatch "[\\/]ThirdParty[\\/]"
                } |
                Sort-Object -Unique
        )
        Write-Step "Running clang-tidy ($($clangTidySources.Count) files)"
        $clangTidyConfig = Join-Path $repositoryRoot ".clang-tidy"
        $clangTidyJobs = [Math]::Max(1, [Math]::Min(4, [Environment]::ProcessorCount))
        $clangTidyStartedAt = Get-Date
        $clangTidyCompleted = 0
        $clangTidyFailures = [System.Collections.Generic.List[object]]::new()
        $clangTidySources | ForEach-Object -Parallel {
            $sourcePath = $_
            $output = @(
                & $using:clangTidyExecutable `
                    --quiet `
                    "--config-file=$using:clangTidyConfig" `
                    -p $using:buildPath `
                    $sourcePath 2>&1
            )
            [pscustomobject]@{
                Path = $sourcePath
                ExitCode = $LASTEXITCODE
                Output = $output
            }
        } -ThrottleLimit $clangTidyJobs | ForEach-Object {
            $result = $_
            ++$clangTidyCompleted
            foreach ($line in $result.Output) {
                Write-Host $line
            }
            if ($result.ExitCode -ne 0) {
                [void]$clangTidyFailures.Add($result)
            }
            if ($clangTidyCompleted -eq 1 -or $clangTidyCompleted % 5 -eq 0 -or
                $clangTidyCompleted -eq $clangTidySources.Count) {
                Write-Host ("  [{0}/{1}] files analyzed" -f $clangTidyCompleted, $clangTidySources.Count)
            }
        }

        $clangTidyElapsed = ((Get-Date) - $clangTidyStartedAt).TotalSeconds
        $clangTidyElapsedText = $clangTidyElapsed.ToString("F1", [System.Globalization.CultureInfo]::InvariantCulture)
        if ($clangTidyFailures.Count -ne 0) {
            Write-Host "[FAILED]" -ForegroundColor Red -NoNewline
            Write-Host (" clang-tidy ({0}/{1} files failed, {2}s)" -f `
                    $clangTidyFailures.Count, $clangTidySources.Count, $clangTidyElapsedText)
            throw "clang-tidy failed for $($clangTidyFailures.Count) source file(s)."
        }
        Write-Host "[PASSED]" -ForegroundColor Green -NoNewline
        Write-Host (" clang-tidy ({0} files, {1}s)" -f $clangTidySources.Count, $clangTidyElapsedText)
    }

    Write-Step "Running C++ unit tests"
    Invoke-Checked -FilePath $ctest -ArgumentList @(
        "--test-dir", $buildPath,
        "--output-on-failure",
        "-C", $Configuration
    )

    $rootManifest = Join-Path $repositoryRoot "Rux.toml"
    if (Test-Path -LiteralPath $rootManifest -PathType Leaf) {
        Write-Step "Checking all Rux workspace packages"
        Invoke-Checked -FilePath $rux -ArgumentList @("--manifest", $rootManifest, "check")

        Write-Step "Linting all Rux workspace packages"
        Invoke-Checked -FilePath $rux -ArgumentList @("--manifest", $rootManifest, "lint")
    }
    else {
        $checkScopes = @(Get-ManifestlessCheckScopes -Root $repositoryRoot)
        if ($checkScopes.Count -eq 0) {
            throw "No Rux manifests found to check or lint."
        }

        Write-Step "Checking all discovered Rux packages"
        foreach ($manifest in $checkScopes) {
            Invoke-Checked -FilePath $rux -ArgumentList @("--manifest", $manifest, "check")
        }

        Write-Step "Linting all discovered Rux packages"
        foreach ($manifest in $checkScopes) {
            Invoke-Checked -FilePath $rux -ArgumentList @("--manifest", $manifest, "lint")
        }
    }

    Write-Step "Running all Rux test packages"
    $testArguments = @("test")
    if ($Configuration -eq "Release") {
        $testArguments += "--release"
    }
    Invoke-Checked -FilePath $rux -ArgumentList $testArguments

    $elapsed = (Get-Date) - $startedAt
    Write-Host ""
    Write-Host ("Test workflow passed in {0:mm\:ss}." -f $elapsed) -ForegroundColor Green
}
finally {
    Pop-Location
}
