function Stop-Script {
    param(
        [Parameter(Mandatory)][string]$Message,
        [int]$ExitCode = 1
    )
    [Console]::Error.WriteLine("error: $Message")
    exit $ExitCode
}

function Invoke-Checked {
    param(
        [Parameter(Mandatory)][string]$FilePath,
        [Parameter()][string[]]$ArgumentList = @()
    )
    & $FilePath @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        $exitCode = $LASTEXITCODE
        $commandName = Split-Path -Leaf $FilePath
        Stop-Script -Message "command '$commandName' failed with exit code $exitCode" -ExitCode $exitCode
    }
}

function Format-Duration {
    param([Parameter(Mandatory)][TimeSpan]$Duration)
    $milliseconds = [Math]::Max(0, [Math]::Round($Duration.TotalMilliseconds))
    if ($milliseconds -lt 1000) {
        return "$milliseconds ms"
    }
    if ($milliseconds -lt 60000) {
        return ($milliseconds / 1000).ToString("0.## 's'", [System.Globalization.CultureInfo]::InvariantCulture)
    }

    $minutes = [Math]::Floor($milliseconds / 60000)
    $seconds = ($milliseconds % 60000) / 1000
    return "$minutes min $($seconds.ToString("0.0 's'", [System.Globalization.CultureInfo]::InvariantCulture))"
}

function Write-Step {
    param([Parameter(Mandatory)][string]$Message)
    Write-Host ""
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Write-Passed {
    param([Parameter(Mandatory)][string]$Message)
    Write-Host "[PASSED]" -ForegroundColor Green -NoNewline
    Write-Host " $Message"
}
