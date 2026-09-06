# Host-side CI operations. FreeBSD guest scripts remain POSIX shell programs.
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('Environment', 'NativeEnvironment', 'ValidateNative', 'FreeBSD', 'PrepareFreeBSD', 'Tidy', 'Benchmark')]
    [string]$Task,
    [Parameter(ValueFromRemainingArguments)][string[]]$Arguments = @()
)
$ErrorActionPreference = 'Stop'
$implementations = @{
    Environment = 'Environment.py'
    NativeEnvironment = 'NativeEnvironment.py'
    ValidateNative = 'ValidateNative.py'
    FreeBSD = 'RunFreeBSD.py'
    PrepareFreeBSD = 'PrepareFreeBSD.py'
    Tidy = 'Verify/TidyShard.py'
    Benchmark = 'Benchmark.py'
}
& python (Join-Path $PSScriptRoot "../CI/$($implementations[$Task])") @Arguments
exit $LASTEXITCODE
