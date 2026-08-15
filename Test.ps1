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

.PARAMETER Target
The target triple to check and run the Rux suites for. Defaults to the host.
Target tests require the same OS and an architecture executable directly by
the compiler process or native OS.

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

.EXAMPLE
./Test.ps1 -SkipBuild -Target windows-aarch64
#>

[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",

    [string]$BuildDirectory = "Build",

    [string]$RuxExecutable,

    [string]$Target,

    [switch]$SkipBuild,

    [switch]$FixFormatting,

    [switch]$ClangTidy
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "Scripts/RepositoryMessages.ps1")

function Find-Tool {
    param(
        [Parameter(Mandatory)]
        [string[]]$Name,

        [string[]]$FallbackPath = @(),

        [string]$Hint
    )

    foreach ($candidate in $Name) {
        $command = Get-Command $candidate -CommandType Application -ErrorAction SilentlyContinue
        if ($command) {
            return $command.Source
        }
    }

    foreach ($candidate in $FallbackPath) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return $candidate
        }
    }

    $message = "required tool '$($Name -join "' or '")' was not found"
    if ($Hint) {
        $message += "; $Hint"
    }
    Stop-Script $message
}

function Get-PosixShellCandidate {
    <#
    .SYNOPSIS
    Well-known sh.exe locations for Git for Windows.

    .DESCRIPTION
    A default Git for Windows installation puts only its cmd directory on PATH,
    so sh.exe is present but not discoverable by name. Derive its location from
    the git executable, then fall back to the standard install roots.
    #>

    $candidates = [System.Collections.Generic.List[string]]::new()

    $git = Get-Command git -CommandType Application -ErrorAction SilentlyContinue
    if ($git) {
        # git.exe lives in <root>\cmd or <root>\bin; sh.exe in <root>\bin or <root>\usr\bin.
        $gitRoot = Split-Path -Parent (Split-Path -Parent $git.Source)
        $candidates.Add((Join-Path $gitRoot "bin\sh.exe"))
        $candidates.Add((Join-Path $gitRoot "usr\bin\sh.exe"))
    }

    $roots = @(
        $env:ProgramFiles
        ${env:ProgramFiles(x86)}
        if ($env:LOCALAPPDATA) { Join-Path $env:LOCALAPPDATA "Programs" }
    )
    foreach ($root in $roots) {
        if ($root) {
            $candidates.Add((Join-Path $root "Git\bin\sh.exe"))
            $candidates.Add((Join-Path $root "Git\usr\bin\sh.exe"))
        }
    }

    return @($candidates)
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
$shell = Find-Tool -Name "sh" `
    -FallbackPath (Get-PosixShellCandidate) `
    -Hint "Install Git for Windows, or add a POSIX shell to PATH."
if ($ClangTidy -and $PSVersionTable.PSVersion.Major -lt 7) {
    Stop-Script "option '-ClangTidy' requires PowerShell 7 or newer"
}
$clangTidyExecutable = if ($ClangTidy) {
    Find-Tool -Name @("clang-tidy-22", "clang-tidy")
}

$startedAt = Get-Date
Push-Location $repositoryRoot
try {
    Write-Step "Checking source-tree policy"
    # A login shell ensures Git for Windows adds dirname and grep to PATH.
    Invoke-Checked -FilePath $shell -ArgumentList @("-lc", "sh Tests/Policy/PlatformIsolation/Check.sh")
    Invoke-Checked -FilePath $shell -ArgumentList @("-lc", "sh Tests/Policy/NoExternalToolchain/Check.sh")
    Invoke-Checked -FilePath $shell -ArgumentList @("-lc", "sh Tests/Policy/OversizedFiles/Test.sh")
    Invoke-Checked -FilePath $shell -ArgumentList @("-lc", "sh Tests/Policy/OversizedFiles/Check.sh")
    Invoke-Checked -FilePath $shell -ArgumentList @("-lc", "sh Tests/Policy/ScriptMessages/Check.sh")
    Invoke-Checked -FilePath $shell -ArgumentList @("-lc", "sh Tests/Policy/InstallerMessages/Check.sh")

    if (-not $SkipBuild) {
        & (Join-Path $repositoryRoot "Build.ps1") `
            -Configuration $Configuration `
            -BuildDirectory $BuildDirectory
    }
    else {
        Write-Step "Skipping compiler build"
        Write-Host "  Note: using the existing build in '$buildPath'"
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
        Stop-Script "rux executable '$rux' was not found; build it first or pass -RuxExecutable"
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
            Stop-Script "compilation database '$compileCommands' was not found; run Test.ps1 without -SkipBuild first"
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

        $clangTidyElapsed = Format-Duration -Duration ((Get-Date) - $clangTidyStartedAt)
        if ($clangTidyFailures.Count -ne 0) {
            Write-Host "[FAILED]" -ForegroundColor Red -NoNewline
            Write-Host (" clang-tidy ({0}/{1} files failed) in {2}" -f `
                    $clangTidyFailures.Count, $clangTidySources.Count, $clangTidyElapsed)
            Stop-Script "clang-tidy failed for $($clangTidyFailures.Count) files"
        }
        Write-Host "[PASSED]" -ForegroundColor Green -NoNewline
        Write-Host (" clang-tidy ({0} files) in {1}" -f $clangTidySources.Count, $clangTidyElapsed)
    }

    Write-Step "Running C++ unit tests"
    Invoke-Checked -FilePath $ctest -ArgumentList @(
        "--test-dir", $buildPath,
        "--output-on-failure",
        "-C", $Configuration
    )

    # `--target` follows the subcommand, while the global `--manifest` precedes
    # it, so it is appended rather than inserted. `lint` takes no target.
    $targetArguments = if ($Target) { @("--target", $Target) } else { @() }

    $rootManifest = Join-Path $repositoryRoot "Rux.toml"
    if (Test-Path -LiteralPath $rootManifest -PathType Leaf) {
        Write-Step "Checking all Rux workspace packages"
        Invoke-Checked -FilePath $rux -ArgumentList (@("--manifest", $rootManifest, "check") + $targetArguments)

        Write-Step "Linting all Rux workspace packages"
        Invoke-Checked -FilePath $rux -ArgumentList @("--manifest", $rootManifest, "lint")
    }
    else {
        $checkScopes = @(Get-ManifestlessCheckScopes -Root $repositoryRoot)
        if ($checkScopes.Count -eq 0) {
            Stop-Script "no Rux manifests were found to check or lint"
        }

        Write-Step "Checking all discovered Rux packages"
        foreach ($manifest in $checkScopes) {
            Invoke-Checked -FilePath $rux -ArgumentList (@("--manifest", $manifest, "check") + $targetArguments)
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
    $testArguments += $targetArguments
    Invoke-Checked -FilePath $rux -ArgumentList $testArguments

    $elapsed = Format-Duration -Duration ((Get-Date) - $startedAt)
    Write-Host ""
    Write-Host "Finished test workflow in $elapsed" -ForegroundColor Green
}
finally {
    Pop-Location
}
