# Download into a temporary file; a failed transfer must never become a cache hit.
function Get-VerifiedDownload {
    param([string]$Url, [string]$Path, [string]$Sha256)
    if ($Sha256 -notmatch '^[a-fA-F0-9]{64}$') { throw 'Invalid SHA-256 pin' }
    if ((Test-Path -LiteralPath $Path) -and
        (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -eq $Sha256) { return }
    $parent = Split-Path -Parent $Path
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    $partial = "$Path.partial"
    try {
        for ($attempt = 1; $attempt -le 3; $attempt++) {
            try {
                Invoke-WebRequest -Uri $Url -OutFile $partial
                break
            } catch {
                if ($attempt -eq 3) { throw }
                Start-Sleep -Seconds (2 * $attempt)
            }
        }
        if ((Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash -ne $Sha256) {
            throw "Checksum mismatch: $Url"
        }
        Move-Item -LiteralPath $partial -Destination $Path -Force
    } finally {
        if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial -Force }
    }
}
