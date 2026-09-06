# CI verification entry point. Invocations never rewrite tracked source files.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('Environment', 'Infrastructure')][string]$Check,
    [string]$Compiler,
    [ValidateSet('amd64', 'arm64', '')][string]$VsArch = ''
)
$ErrorActionPreference = 'Stop'
switch ($Check) {
    'Environment' { & "$PSScriptRoot/../CI/Verify/CacheIdentity.ps1" -Compiler $Compiler -VsArch $VsArch }
    'Infrastructure' {
        & python "$PSScriptRoot/../../Tests/Scripts/CI/Check.py"
        if ($LASTEXITCODE -ne 0) { throw 'CI infrastructure checks failed' }
    }
}
