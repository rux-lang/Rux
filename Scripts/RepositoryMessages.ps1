# Shared human-output helpers for the PowerShell repository entry point.

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
        [Parameter()][string[]]$ArgumentList = @(),
        # Reported instead of the executable name when the command stands for
        # something more specific, such as one policy guard run through sh.
        [string]$Name
    )
    & $FilePath @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        $exitCode = $LASTEXITCODE
        $commandName = if ($Name) { $Name } else { Split-Path -Leaf $FilePath }
        Stop-Script -Message "command '$commandName' failed with exit code $exitCode" -ExitCode $exitCode
    }
}

function Find-Tool {
    <#
    .SYNOPSIS
    Resolves the first available executable from a candidate list.

    .DESCRIPTION
    Candidates are looked up on PATH in order, then as literal file paths in
    FallbackPath. NotFoundMessage replaces the generated failure text when a
    caller owns a more specific wording.
    #>

    param(
        [Parameter(Mandatory)][string[]]$Name,
        [string[]]$FallbackPath = @(),
        [string]$Hint = "install it and ensure it is available on PATH",
        [string]$NotFoundMessage
    )

    foreach ($candidate in $Name) {
        # PATH may hold several copies (the Visual Studio environment adds its
        # own CMake); take the first, as the shell would.
        $command = Get-Command $candidate -CommandType Application -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($command) {
            return $command.Source
        }
    }

    foreach ($candidate in $FallbackPath) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return $candidate
        }
    }

    if ($NotFoundMessage) {
        Stop-Script $NotFoundMessage
    }

    $message = "required tool '$($Name -join "' or '")' was not found"
    if ($Hint) {
        $message += "; $Hint"
    }
    Stop-Script $message
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

function Test-ColorSupport {
    if ($env:NO_COLOR) {
        return $false
    }
    return -not [Console]::IsOutputRedirected
}

function Write-Colored {
    param(
        [Parameter(Mandatory)][string]$Text,
        [Parameter(Mandatory)][string]$Color,
        [string]$Suffix = ""
    )
    if (Test-ColorSupport) {
        Write-Host $Text -ForegroundColor $Color -NoNewline
        Write-Host $Suffix
    }
    else {
        Write-Host "$Text$Suffix"
    }
}

function Write-Step {
    param([Parameter(Mandatory)][string]$Message)
    Write-Host ""
    Write-Colored -Text "==> $Message" -Color Cyan
}

function Write-Status {
    <#
    .SYNOPSIS
    Prints a status line the way the compiler does: a colored verb, then the detail.

    .DESCRIPTION
    The verb takes the console color rather than an escape sequence: a console
    that has not enabled virtual-terminal processing shows raw sequences, while
    the console color renders everywhere the script runs.
    #>
    param(
        [Parameter(Mandatory)][string]$Verb,
        [Parameter(Mandatory)][string]$Color,
        [string]$Detail = ""
    )
    Write-Colored -Text $Verb -Color $Color -Suffix $(if ($Detail) { " $Detail" } else { "" })
}

function Write-Passed {
    param([Parameter(Mandatory)][string]$Message)
    Write-Status -Verb "Passed" -Color Green -Detail $Message
}

function Write-Failed {
    param([Parameter(Mandatory)][string]$Message)
    Write-Status -Verb "Failed" -Color Red -Detail $Message
}

function Write-Finished {
    param([Parameter(Mandatory)][string]$Message)
    Write-Host ""
    Write-Colored -Text $Message -Color Green
}
