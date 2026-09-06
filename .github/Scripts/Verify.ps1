<#
.SYNOPSIS
    Builds and verifies Rux on one Windows target.

.DESCRIPTION
    The Windows peer of .github/Scripts/Verify.sh. Everything it does goes
    through ./Run.ps1, the same entry point developers use, so a green job means
    the developer workflow is green.

.PARAMETER Stage
    build, test, or closure.

.EXAMPLE
    ./.github/Scripts/Verify.ps1 -Stage build
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('build', 'test', 'closure')]
    [string] $Stage
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$jobs = [Math]::Max(1, [Math]::Min(4, [Environment]::ProcessorCount))
$rux = './Bin/rux.exe'

function Invoke-Native {
    param([Parameter(Mandatory)][scriptblock] $Command)
    & $Command
    if ($LASTEXITCODE -ne 0) { throw "command failed with exit code $LASTEXITCODE" }
}

switch ($Stage) {
    'build' {
        # PCH is disabled so ccache sees ordinary translation units; the two
        # defeat each other otherwise.
        Invoke-Native { ./Run.ps1 build -NoPch }
    }
    'test' {
        Invoke-Native { ./Run.ps1 unit -Jobs $jobs }
        Invoke-Native { & $rux check }
        Invoke-Native { & $rux lint }
        Invoke-Native { & $rux test --release --jobs $jobs }
    }
    'closure' {
        # The shipped compiler must depend only on system DLLs: a toolchain that
        # is present on the runner is not present on a user's machine.
        $dependencies = @(
            & dumpbin /nologo /dependents $rux |
                Select-String -Pattern '^\s{4}(\S+\.dll)\s*$' |
                ForEach-Object { $_.Matches[0].Groups[1].Value }
        )
        if ($dependencies.Count -eq 0) { throw "could not read the imports of '$rux'" }

        $unexpected = @($dependencies | Where-Object {
                $_ -notmatch '^(?i)(KERNEL32|ADVAPI32|USER32|SHELL32|ole32|OLEAUT32|bcrypt|ntdll|VCRUNTIME\d*|api-ms-win-|ucrtbase|MSVCP\d*)'
            })
        if ($unexpected.Count -ne 0) {
            throw "'$rux' imports non-system DLLs: $($unexpected -join ', ')"
        }
        Write-Host "$rux depends only on system DLLs"
    }
}
