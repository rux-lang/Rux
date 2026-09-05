<#
.SYNOPSIS
Repository entry point for local development on Windows.

.DESCRIPTION
Run.ps1 dispatches one command per invocation:

  build     Configure and build the compiler and C++ unit tests
  test      Run the complete verification workflow
  format    Format C++ and Rux sources, or check them with -Check
  policy    Run the source-tree policy guards
  tidy      Run clang-tidy over maintained C++ sources
  unit      Run the C++ unit tests through CTest
  clean     Remove the build directory and Bin
  help      Show the command and option summary

Running the script without a command prints the same summary. Every command is
a step of the test workflow, so a step behaves identically alone and in the
workflow. sh Run.sh is the equivalent entry point on Linux, macOS, and FreeBSD.

.PARAMETER Command
The command to run. Defaults to help.

.PARAMETER Configuration
The CMake configuration to build and test, for build, test, and unit. Defaults
to Release.

.PARAMETER BuildDirectory
The CMake build directory, relative to the repository root unless absolute, for
build, test, unit, tidy, and clean.

.PARAMETER Compiler
The Clang C++ compiler to configure with, for build and test. Defaults to $CXX
or a detected Clang 23.

.PARAMETER RuxExecutable
An existing rux executable to use, for format and test. Defaults to Bin/rux or
Bin/rux.exe.

.PARAMETER Target
The target triple to check and run the Rux suites for, for test. Defaults to
the host. Target tests require the same OS and an architecture executable
directly by the compiler process or native OS.

.PARAMETER Jobs
Maximum workers for tests and static analysis (default: up to four CPUs).
An explicit value also limits C++ build workers.

.PARAMETER Check
Checks formatting without modifying files, for format.

.PARAMETER FixFormatting
Formats C++ and Rux sources in place instead of only checking them, for test.

.PARAMETER SkipBuild
Reuses the existing build and executables, for test.

.PARAMETER ClangTidy
Adds the clang-tidy pass to the workflow, for test. Clang-tidy is optional
because a full static-analysis pass is comparatively slow, and it requires
PowerShell 7 because it analyzes files in parallel.

.EXAMPLE
./Run.ps1 build

.EXAMPLE
./Run.ps1 build -Configuration Debug -BuildDirectory Build-Debug

.EXAMPLE
./Run.ps1 format -Check

.EXAMPLE
./Run.ps1 test -SkipBuild -ClangTidy

.EXAMPLE
./Run.ps1 test -SkipBuild -Target windows-aarch64
#>

[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet("build", "test", "format", "policy", "tidy", "unit", "clean", "help")]
    [string]$Command = "help",

    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",

    [string]$BuildDirectory = "Build",

    [string]$Compiler,

    [string]$RuxExecutable,

    [string]$Target,

    [ValidateRange(1, 2147483647)]
    [int]$Jobs = [Math]::Max(1, [Math]::Min(4, [Environment]::ProcessorCount)),

    [switch]$Check,

    [switch]$FixFormatting,

    [switch]$SkipBuild,

    [switch]$ClangTidy
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "Scripts/RepositoryMessages.ps1")

$repositoryRoot = $PSScriptRoot
$runningOnWindows = [System.Environment]::OSVersion.Platform -eq [System.PlatformID]::Win32NT

$policyChecks = @(
    "Tests/Policy/PlatformIsolation/Check.sh",
    "Tests/Policy/NoExternalToolchain/Check.sh",
    "Tests/Policy/OutputOwnership/Test.sh",
    "Tests/Policy/OutputOwnership/Check.sh"
)

# Options each command accepts, used to reject an option the command ignores.
$commandOptions = @{
    build  = @("Configuration", "BuildDirectory", "Compiler", "Jobs")
    test   = @("Configuration", "BuildDirectory", "Compiler", "RuxExecutable", "Target",
        "FixFormatting", "SkipBuild", "ClangTidy", "Jobs")
    format = @("RuxExecutable", "Check")
    policy = @()
    tidy   = @("BuildDirectory", "Jobs")
    unit   = @("Configuration", "BuildDirectory", "Jobs")
    clean  = @("BuildDirectory")
    help   = @()
}

function Show-Usage {
    Write-Host "Usage: ./Run.ps1 <command> [options]"
    Write-Host ""
    Write-Host "Commands:"
    Write-Host "  build     Configure and build the compiler and C++ unit tests"
    Write-Host "  test      Run the complete verification workflow"
    Write-Host "  format    Format C++ and Rux sources, or check them with -Check"
    Write-Host "  policy    Run the source-tree policy guards"
    Write-Host "  tidy      Run clang-tidy over maintained C++ sources"
    Write-Host "  unit      Run the C++ unit tests through CTest"
    Write-Host "  clean     Remove the build directory and Bin"
    Write-Host "  help      Show this help"
    Write-Host ""
    Write-Host "Options:"
    Write-Host "  -Jobs N                        Test/tidy workers (default: up to four CPUs); explicit value also limits builds"
    Write-Host "  -Configuration Debug|Release  CMake configuration (build, test, unit; default: Release)"
    Write-Host "  -BuildDirectory PATH          CMake build directory (build, test, unit, tidy, clean; default: Build)"
    Write-Host "  -Compiler PATH                Clang C++ compiler (build, test; default: `$CXX or detected Clang)"
    Write-Host "  -RuxExecutable PATH           Existing rux executable (format, test; default: Bin/rux.exe)"
    Write-Host "  -Target TRIPLE                Check and directly run suites for this target (test; default: the host)"
    Write-Host "  -Check                        Check formatting without modifying files (format)"
    Write-Host "  -FixFormatting                Format sources instead of checking them (test)"
    Write-Host "  -SkipBuild                    Reuse the existing build and executables (test)"
    Write-Host "  -ClangTidy                    Add the clang-tidy pass (test)"
    Write-Host ""
    Write-Host "Examples:"
    Write-Host "  ./Run.ps1 build -Configuration Debug"
    Write-Host "  ./Run.ps1 format -Check"
    Write-Host "  ./Run.ps1 test -SkipBuild -ClangTidy"
}

function Get-PosixShell {
    <#
    .SYNOPSIS
    Resolves a POSIX shell for the repository policy guards.

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

    return Find-Tool -Name "sh" `
        -FallbackPath @($candidates) `
        -Hint "Install Git for Windows, or add a POSIX shell to PATH."
}

function Get-BuildPath {
    if ([System.IO.Path]::IsPathRooted($BuildDirectory)) {
        return $BuildDirectory
    }
    return (Join-Path $repositoryRoot $BuildDirectory)
}

function Get-RuxPath {
    if ($RuxExecutable) {
        if ([System.IO.Path]::IsPathRooted($RuxExecutable)) {
            return $RuxExecutable
        }
        return (Join-Path $repositoryRoot $RuxExecutable)
    }

    $ruxFileName = if ($runningOnWindows) { "rux.exe" } else { "rux" }
    return (Join-Path $repositoryRoot "Bin/$ruxFileName")
}

function Resolve-RuxPath {
    $rux = Get-RuxPath
    if (-not (Test-Path -LiteralPath $rux -PathType Leaf)) {
        Stop-Script "rux executable '$rux' was not found; build it first or pass -RuxExecutable"
    }
    return $rux
}

function Resolve-Compiler {
    $requested = if ($Compiler) { $Compiler } elseif ($env:CXX) { $env:CXX } else { "" }
    if ($requested) {
        return Find-Tool -Name $requested `
            -FallbackPath @($requested) `
            -NotFoundMessage "C++ compiler '$requested' was not found; install Clang 23 or pass -Compiler PATH"
    }

    return Find-Tool -Name @("clang++-23", "clang++23", "clang++") `
        -NotFoundMessage "Clang 23 was not found; install it or pass -Compiler PATH"
}

function Get-MaintainedCppFile {
    $files = @(
        Get-ChildItem -LiteralPath (Join-Path $repositoryRoot "Compiler"), (Join-Path $repositoryRoot "Tests/Unit") `
            -File -Recurse |
            Where-Object {
                $_.Extension -in ".cpp", ".h" -and
                $_.FullName -notmatch "[\\/]ThirdParty[\\/]"
            } |
            Sort-Object FullName
    )

    if ($files.Count -eq 0) {
        Stop-Script "no maintained C++ files were found"
    }
    return $files
}

function Get-PackageManifest {
    $manifests = @(
        Get-ChildItem -LiteralPath (Join-Path $repositoryRoot "Packages"), (Join-Path $repositoryRoot "Tests") `
            -File -Recurse -Filter "Rux.toml" |
            Sort-Object FullName
    )

    if ($manifests.Count -eq 0) {
        Stop-Script "no Rux package or test manifests were found"
    }
    return $manifests
}

function Invoke-Filtered {
    <#
    .SYNOPSIS
    Runs a command with each output line reshaped by a filter.

    .DESCRIPTION
    The filter receives one line and returns the line to print, or nothing to
    drop it; output streams as the command produces it. A failure is reported
    the way Invoke-Checked reports one.
    #>

    param(
        [Parameter(Mandatory)][string]$FilePath,
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][scriptblock]$Filter,
        [string[]]$ArgumentList = @()
    )

    # Redirected stderr lines are part of the report, not terminating errors (Windows PowerShell under Stop).
    $ErrorActionPreference = "Continue"
    & $FilePath @ArgumentList 2>&1 | ForEach-Object {
        $rendered = & $Filter "$_"
        if ($null -ne $rendered) {
            Write-Host $rendered
        }
    }
    if ($LASTEXITCODE -ne 0) {
        Stop-Script -Message "command '$Name' failed with exit code $LASTEXITCODE" -ExitCode $LASTEXITCODE
    }
}

function Invoke-ClangFormat {
    <#
    .SYNOPSIS
    Runs clang-format over the given files in batches.

    .DESCRIPTION
    Batching keeps the process count low without approaching the command-line
    length limit, which one invocation for the whole tree would exceed.
    #>

    param(
        [Parameter(Mandatory)][string]$ClangFormat,
        [Parameter(Mandatory)][string[]]$ArgumentList,
        [Parameter(Mandatory)][string[]]$Path
    )

    $batchSize = 64
    for ($index = 0; $index -lt $Path.Count; $index += $batchSize) {
        $last = [Math]::Min($index + $batchSize, $Path.Count) - 1
        Invoke-Checked -FilePath $ClangFormat -ArgumentList ($ArgumentList + @($Path[$index..$last]))
    }
}

function Invoke-Policy {
    $shell = Get-PosixShell

    Write-Step "Checking source-tree policy"
    foreach ($check in $policyChecks) {
        # A login shell ensures Git for Windows adds dirname and grep to PATH.
        Invoke-Checked -FilePath $shell `
            -ArgumentList @("-lc", "sh $check") `
            -Name (Split-Path -Leaf (Split-Path -Parent $check))
    }
}

function Invoke-Build {
    $buildPath = Get-BuildPath
    $cmake = Find-Tool -Name "cmake"
    $null = Find-Tool -Name "ninja"
    $compilerPath = Resolve-Compiler

    $configureArguments = [System.Collections.Generic.List[string]]::new()
    $configureArguments.AddRange([string[]]@(
            "-S", $repositoryRoot,
            "-B", $buildPath,
            "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=$Configuration",
            "-DCMAKE_CXX_COMPILER=$compilerPath"
        ))
    if ($runningOnWindows) {
        # Clang defaults to the GNU target on Windows; the MSVC triple matches
        # the platform runtime the compiler and its tests link against.
        $compilerTarget = switch ([System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture) {
            ([System.Runtime.InteropServices.Architecture]::X64) { "x86_64-pc-windows-msvc" }
            ([System.Runtime.InteropServices.Architecture]::Arm64) { "aarch64-pc-windows-msvc" }
            default {
                Stop-Script ("Windows architecture " +
                    "'$([System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture)' is not supported")
            }
        }
        $configureArguments.Add("-DCMAKE_CXX_COMPILER_TARGET=$compilerTarget")
    }
    $configureArguments.AddRange([string[]]@(
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            "-DRUX_WERROR=ON",
            "-DRUX_BUILD_TESTS=ON"
        ))

    # CMake's configure report: `-- Configuring done (0.1s)` becomes `Configuring done in 100 ms`, the
    # build-files line becomes a detail, and other status lines lose their `-- ` prefix. Anything else
    # passes through.
    $configureFilter = {
        param([string]$Line)
        if ($Line -match '^-- (Configuring|Generating) done \(([0-9.]+)s\)$') {
            $elapsed = Format-Duration -Duration ([TimeSpan]::FromSeconds([double]$Matches[2]))
            return "$($Matches[1]) done in $elapsed"
        }
        if ($Line -match '^-- Build files have been written to: (.+)$') {
            return "  Build files: '$($Matches[1])'"
        }
        if ($Line -match '^-- (.*)$') {
            return $Matches[1]
        }
        return $Line
    }
    # Ninja progress: `[3/414] Building CXX object .../CMakeFiles/RuxSystem.dir/Process.cpp.obj` becomes
    # `Compiling Compiler/System/Process.cpp (3/414)`, a link names its output, and an up-to-date tree
    # says so. Diagnostics pass through unchanged.
    $buildFilter = {
        param([string]$Line)
        if ($Line -match '^\[([0-9]+/[0-9]+)\] (.*)$') {
            $progress = "($($Matches[1]))"
            $action = $Matches[2]
            if ($action -match '^Building CXX object (.*)$') {
                $source = $Matches[1] -replace 'CMakeFiles/[^/]+\.dir/', '' -replace '\.(obj|o)$', '' -replace '__/', '../'
                return "Compiling $source $progress"
            }
            if ($action -match '^Linking CXX (?:executable|static library|shared library) (.*)$') {
                return "Linking '$($Matches[1])' $progress"
            }
            return "$action $progress"
        }
        if ($Line -eq "ninja: no work to do.") {
            return "Up to date"
        }
        return $Line
    }

    $startedAt = Get-Date
    Write-Step "Configuring $Configuration build"
    Invoke-Filtered -FilePath $cmake -Name "cmake" -Filter $configureFilter -ArgumentList $configureArguments

    Write-Step "Building compiler and unit tests"
    $buildArguments = @("--build", $buildPath, "--config", $Configuration)
    if ($script:PSBoundParameters.ContainsKey("Jobs")) { $buildArguments += @("--parallel", "$Jobs") }
    Invoke-Filtered -FilePath $cmake -Name "cmake" -Filter $buildFilter -ArgumentList $buildArguments

    $ruxFileName = if ($runningOnWindows) { "rux.exe" } else { "rux" }
    $rux = Join-Path $repositoryRoot "Bin/$ruxFileName"
    if (-not (Test-Path -LiteralPath $rux -PathType Leaf)) {
        Stop-Script "build completed without producing the expected compiler at '$rux'"
    }

    Write-Finished "Finished build in $(Format-Duration -Duration ((Get-Date) - $startedAt))"
    Write-Host "  Output: '$rux'"
}

function Invoke-Format {
    <#
    .SYNOPSIS
    Formats maintained C++ and Rux sources, or checks them with -CheckOnly.

    .DESCRIPTION
    Runs clang-format over Compiler/ and Tests/Unit/ (excluding vendored code),
    then runs rux fmt for every package and executable test manifest. Golden
    diagnostic fixtures are intentionally excluded because malformed formatting
    is part of what they test.
    #>

    param([switch]$CheckOnly)

    $clangFormat = Find-Tool -Name @("clang-format-23", "clang-format23", "clang-format")
    $rux = Resolve-RuxPath
    $cppFiles = @(Get-MaintainedCppFile | ForEach-Object { $_.FullName })
    $manifests = @(Get-PackageManifest | ForEach-Object { $_.FullName })

    $startedAt = Get-Date
    if ($CheckOnly) {
        Write-Step "Checking C++ formatting ($($cppFiles.Count) files)"
        Invoke-ClangFormat -ClangFormat $clangFormat -ArgumentList @("--dry-run", "-Werror") -Path $cppFiles
        Write-Passed "C++ formatting ($($cppFiles.Count) files)"

        Write-Step "Checking Rux formatting ($($manifests.Count) packages)"
        foreach ($manifest in $manifests) {
            Invoke-Checked -FilePath $rux -ArgumentList @("--manifest", $manifest, "fmt", "--check")
        }
        Write-Passed "Rux formatting ($($manifests.Count) packages)"

        Write-Finished "Finished format check in $(Format-Duration -Duration ((Get-Date) - $startedAt))"
    }
    else {
        Write-Step "Formatting C++ sources ($($cppFiles.Count) files)"
        Invoke-ClangFormat -ClangFormat $clangFormat -ArgumentList @("-i") -Path $cppFiles
        Write-Passed "C++ formatting ($($cppFiles.Count) files)"

        Write-Step "Formatting Rux sources ($($manifests.Count) packages)"
        foreach ($manifest in $manifests) {
            Invoke-Checked -FilePath $rux -ArgumentList @("--manifest", $manifest, "fmt")
        }
        Write-Passed "Rux formatting ($($manifests.Count) packages)"

        Write-Finished "Finished source formatting in $(Format-Duration -Duration ((Get-Date) - $startedAt))"
    }
}

function Invoke-Tidy {
    if ($PSVersionTable.PSVersion.Major -lt 7) {
        Stop-Script "running clang-tidy requires PowerShell 7 or newer"
    }

    $buildPath = Get-BuildPath
    $clangTidy = Find-Tool -Name @("clang-tidy-23", "clang-tidy")
    $cache = Join-Path $buildPath "CMakeCache.txt"
    if ((Test-Path -LiteralPath $cache) -and (Select-String -LiteralPath $cache -Pattern '^RUX_USE_PCH:BOOL=ON$' -Quiet)) {
        Invoke-Checked -FilePath (Find-Tool -Name "cmake") -ArgumentList @("--build", $buildPath, "--target", "rux-analysis-database")
        $buildPath = Join-Path $buildPath "Analysis"
    }
    $compileCommands = Join-Path $buildPath "compile_commands.json"
    if (-not (Test-Path -LiteralPath $compileCommands -PathType Leaf)) {
        Stop-Script "compilation database '$compileCommands' was not found; build the compiler first"
    }

    $compilerRoot = (Join-Path $repositoryRoot "Compiler") + [System.IO.Path]::DirectorySeparatorChar
    $unitTestRoot = (Join-Path $repositoryRoot "Tests/Unit") + [System.IO.Path]::DirectorySeparatorChar
    $sources = @(
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

    if ($sources.Count -eq 0) {
        Stop-Script "no maintained C++ translation units were found in '$compileCommands'"
    }

    Write-Step "Running clang-tidy ($($sources.Count) files)"
    $clangTidyConfig = Join-Path $repositoryRoot ".clang-tidy"
    $startedAt = Get-Date
    $completed = 0
    $failures = [System.Collections.Generic.List[object]]::new()
    $sources | ForEach-Object -Parallel {
        $sourcePath = $_
        $output = @(
            & $using:clangTidy `
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
    } -ThrottleLimit $jobs | ForEach-Object {
        $result = $_
        ++$completed
        # Files complete out of submission order, so the count is progress
        # through the run rather than the position of this file in the list.
        $label = "{0} ({1}/{2})" -f `
            $result.Path.Substring($repositoryRoot.Length + 1), $completed, $sources.Count
        if ($result.ExitCode -ne 0) {
            [void]$failures.Add($result)
            Write-Failed $label
        }
        else {
            Write-Passed $label
        }
        foreach ($line in $result.Output) {
            Write-Host $line
        }
    }

    $elapsed = Format-Duration -Duration ((Get-Date) - $startedAt)
    if ($failures.Count -ne 0) {
        Write-Failed ("clang-tidy ({0}/{1} files failed) in {2}" -f $failures.Count, $sources.Count, $elapsed)
        Stop-Script "clang-tidy failed for $($failures.Count) files"
    }
    Write-Passed ("clang-tidy ({0} files) in {1}" -f $sources.Count, $elapsed)
}

function Format-TestCount {
    param([Parameter(Mandatory)][int]$Count)
    if ($Count -eq 1) { return "1 test" }
    return "$Count tests"
}

function Invoke-Unit {
    $buildPath = Get-BuildPath
    $ctest = Find-Tool -Name "ctest"

    Write-Step "Running C++ unit tests"
    # Reshapes CTest's report into the `rux test` vocabulary: one line per group
    # as it completes, then a total. A failing group's output
    # (--output-on-failure) and anything unrecognized pass through unchanged.
    $startedAt = Get-Date
    $passed = 0
    $failed = 0
    $announced = $false
    $summarizing = $false
    # Windows PowerShell turns redirected stderr lines into terminating errors under Stop; they are ordinary
    # lines of the report here. The preference is local to this function.
    $ErrorActionPreference = "Continue"
    & $ctest --test-dir $buildPath --output-on-failure -C $Configuration --parallel "$Jobs" --no-tests=error 2>&1 |
        ForEach-Object {
            $line = "$_"
            if ($line -match '^ *[0-9]+/([0-9]+) Test +#[0-9]+: ([^ ]+) +[.]* *(.*?) +([0-9]+[.][0-9]+) sec *$') {
                if (-not $announced) {
                    $announced = $true
                    Write-Status -Verb "Running" -Color Cyan -Detail (Format-TestCount ([int]$Matches[1]))
                }
                $state = ($Matches[3].TrimStart('*') -replace '  +', ' ').Trim()
                $duration = Format-Duration -Duration ([TimeSpan]::FromSeconds([double]$Matches[4]))
                $detail = "$($Matches[2]) in $duration"
                if ($state -eq "Passed") {
                    ++$passed
                    Write-Passed $detail
                }
                else {
                    ++$failed
                    if ($state -ne "Failed") {
                        $detail += " ($state)"
                    }
                    Write-Failed $detail
                }
            }
            elseif ($line -match '^[0-9]+% tests passed') {
                $summarizing = $true
            }
            elseif (-not $summarizing -and $line -notmatch '^(Test project | *Start +[0-9]+: | *$)') {
                Write-Host $line
            }
        }
    $exitCode = $LASTEXITCODE
    if ($passed + $failed -gt 0) {
        $elapsed = Format-Duration -Duration ((Get-Date) - $startedAt)
        $totals = "{0} in {1} ({2} passed, {3} failed)" -f (Format-TestCount ($passed + $failed)), $elapsed, $passed, $failed
        if ($exitCode -eq 0) {
            Write-Passed $totals
        }
        else {
            Write-Failed $totals
        }
    }
    if ($exitCode -ne 0) {
        Stop-Script -Message "command 'ctest' failed with exit code $exitCode" -ExitCode $exitCode
    }
}

function Invoke-RuxSuite {
    $rux = Resolve-RuxPath
    $rootManifest = Join-Path $repositoryRoot "Rux.toml"
    if (-not (Test-Path -LiteralPath $rootManifest -PathType Leaf)) {
        Stop-Script "workspace manifest '$rootManifest' was not found"
    }

    # `--target` follows the subcommand, while the global `--manifest` precedes
    # it, so it is appended rather than inserted. `lint` takes no target.
    $targetArguments = if ($Target) { @("--target", $Target) } else { @() }

    Write-Step "Checking all Rux workspace packages"
    Invoke-Checked -FilePath $rux -ArgumentList (@("--manifest", $rootManifest, "check") + $targetArguments)

    Write-Step "Linting all Rux workspace packages"
    Invoke-Checked -FilePath $rux -ArgumentList @("--manifest", $rootManifest, "lint")

    Write-Step "Running all Rux test packages"
    $testArguments = @("test", "--jobs", "$Jobs")
    if ($Configuration -eq "Release") {
        $testArguments += "--release"
    }
    $testArguments += $targetArguments
    Invoke-Checked -FilePath $rux -ArgumentList $testArguments
}

function Invoke-Clean {
    $buildPath = Get-BuildPath

    Write-Step "Removing build outputs"
    foreach ($path in @($buildPath, (Join-Path $repositoryRoot "Bin"))) {
        if (Test-Path -LiteralPath $path) {
            Remove-Item -LiteralPath $path -Recurse -Force
            Write-Host "  Removed '$path'"
        }
        else {
            Write-Host "  Skipped '$path' (not present)"
        }
    }
}

function Invoke-TestWorkflow {
    Invoke-Policy

    if (-not $SkipBuild) {
        Invoke-Build
    }
    else {
        Write-Step "Skipping compiler build"
        Write-Host "  note: using the existing build in '$(Get-BuildPath)'"
    }

    if ($FixFormatting) {
        Invoke-Format
    }
    else {
        Invoke-Format -CheckOnly
    }

    if ($ClangTidy) {
        Invoke-Tidy
    }

    Invoke-Unit
    Invoke-RuxSuite
}

if ($Command -eq "help") {
    Show-Usage
    exit 0
}

$allOptions = @("Configuration", "BuildDirectory", "Compiler", "RuxExecutable", "Target",
    "Check", "FixFormatting", "SkipBuild", "ClangTidy", "Jobs")
foreach ($name in $PSBoundParameters.Keys) {
    if ($allOptions -notcontains $name) {
        continue
    }
    if ($commandOptions[$Command] -notcontains $name) {
        Stop-Script "option '-$name' is not valid for command '$Command'"
    }
}

$startedAt = Get-Date
Push-Location $repositoryRoot
try {
    switch ($Command) {
        "build" {
            Invoke-Build
        }
        "format" {
            if ($Check) {
                Invoke-Format -CheckOnly
            }
            else {
                Invoke-Format
            }
        }
        "policy" {
            Invoke-Policy
            Write-Finished "Finished policy checks in $(Format-Duration -Duration ((Get-Date) - $startedAt))"
        }
        "tidy" {
            Invoke-Tidy
        }
        "unit" {
            Invoke-Unit
            Write-Finished "Finished unit tests in $(Format-Duration -Duration ((Get-Date) - $startedAt))"
        }
        "clean" {
            Invoke-Clean
            Write-Finished "Finished clean in $(Format-Duration -Duration ((Get-Date) - $startedAt))"
        }
        "test" {
            Invoke-TestWorkflow
            Write-Finished "Finished test workflow in $(Format-Duration -Duration ((Get-Date) - $startedAt))"
        }
    }
}
finally {
    Pop-Location
}
