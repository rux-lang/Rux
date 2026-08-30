#Requires -Version 5.1
<#
.SYNOPSIS
Replaces the local Rux package cache with the repository packages.

.DESCRIPTION
Deletes every package below the local Rux package cache, then copies each
first-party package from Packages/ to:

    <CacheRoot>\<Namespace>\<Name>\<Version>\

This is a temporary development helper for using first-party packages before
they are published to the registry.

.PARAMETER CacheRoot
Package-cache directory to replace. Defaults to
%LocalAppData%\Rux\Packages. An alternate directory is useful for testing.

.EXAMPLE
./Scripts/SyncLocalPackages.ps1

.EXAMPLE
./Scripts/SyncLocalPackages.ps1 -WhatIf
#>
[CmdletBinding(SupportsShouldProcess)]
param(
    [string]$CacheRoot = (Join-Path $env:LOCALAPPDATA "Rux\Packages")
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-ManifestString {
    param(
        [Parameter(Mandatory)]
        [string]$ManifestPath,

        [Parameter(Mandatory)]
        [string]$Name
    )

    $pattern = '^\s*' + [Regex]::Escape($Name) + '\s*=\s*"([^"]+)"\s*(?:#.*)?$'
    $matches = @(Select-String -LiteralPath $ManifestPath -Pattern $pattern)
    if ($matches.Count -ne 1) {
        throw "manifest '$ManifestPath' must contain exactly one quoted '$Name' field"
    }

    return $matches[0].Matches[0].Groups[1].Value
}

if (-not $env:LOCALAPPDATA -and -not $PSBoundParameters.ContainsKey("CacheRoot")) {
    throw "LOCALAPPDATA is not set; pass -CacheRoot explicitly"
}

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$sourceRoot = Join-Path $repositoryRoot "Packages"
$resolvedSourceRoot = [IO.Path]::GetFullPath($sourceRoot).TrimEnd('\', '/')
$resolvedCacheRoot = [IO.Path]::GetFullPath($CacheRoot).TrimEnd('\', '/')
$volumeRoot = [IO.Path]::GetPathRoot($resolvedCacheRoot).TrimEnd('\', '/')

if (-not (Test-Path -LiteralPath $resolvedSourceRoot -PathType Container)) {
    throw "repository package directory '$resolvedSourceRoot' does not exist"
}
if (-not $resolvedCacheRoot -or $resolvedCacheRoot -eq $volumeRoot) {
    throw "refusing to use volume root '$resolvedCacheRoot' as the package cache"
}
if ($resolvedCacheRoot -eq $resolvedSourceRoot -or
    $resolvedSourceRoot.StartsWith("$resolvedCacheRoot\", [StringComparison]::OrdinalIgnoreCase) -or
    $resolvedCacheRoot.StartsWith("$resolvedSourceRoot\", [StringComparison]::OrdinalIgnoreCase)) {
    throw "package cache '$resolvedCacheRoot' cannot overlap the repository package directory"
}

# Read and validate every identity before changing the cache. The manifest
# schema restricts these values to safe, single path segments.
$identityPattern = '^[A-Za-z0-9]+(?:[-_][A-Za-z0-9]+)*$'
$versionPattern = '^[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$'
$packages = @(
    Get-ChildItem -LiteralPath $resolvedSourceRoot -Directory | ForEach-Object {
        $manifestPath = Join-Path $_.FullName "Rux.toml"
        if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
            throw "package directory '$($_.FullName)' does not contain Rux.toml"
        }

        $namespace = Get-ManifestString -ManifestPath $manifestPath -Name "Namespace"
        $packageName = Get-ManifestString -ManifestPath $manifestPath -Name "Name"
        $version = Get-ManifestString -ManifestPath $manifestPath -Name "Version"
        if ($namespace -notmatch $identityPattern) {
            throw "manifest '$manifestPath' has an invalid package namespace '$namespace'"
        }
        if ($packageName -notmatch $identityPattern) {
            throw "manifest '$manifestPath' has an invalid package name '$packageName'"
        }
        if ($version -notmatch $versionPattern) {
            throw "manifest '$manifestPath' has an invalid package version '$version'"
        }

        [PSCustomObject]@{
            Source = $_.FullName
            Namespace = $namespace
            Name = $packageName
            Version = $version
        }
    }
)

if ($packages.Count -eq 0) {
    throw "no packages were found below '$resolvedSourceRoot'"
}

$duplicate = $packages |
    Group-Object { "$($_.Namespace)/$($_.Name)/$($_.Version)".ToLowerInvariant().Replace('_', '-') } |
    Where-Object Count -gt 1 |
    Select-Object -First 1
if ($duplicate) {
    throw "repository contains duplicate package identity '$($duplicate.Name)'"
}

if (-not $PSCmdlet.ShouldProcess($resolvedCacheRoot, "replace all cached packages with $($packages.Count) repository packages")) {
    return
}

if (Test-Path -LiteralPath $resolvedCacheRoot) {
    Remove-Item -LiteralPath $resolvedCacheRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $resolvedCacheRoot -Force | Out-Null

foreach ($package in $packages) {
    $packageParent = Join-Path $resolvedCacheRoot $package.Namespace
    $packageParent = Join-Path $packageParent $package.Name
    New-Item -ItemType Directory -Path $packageParent -Force | Out-Null

    $destination = Join-Path $packageParent $package.Version
    Copy-Item -LiteralPath $package.Source -Destination $destination -Recurse
    Write-Host "Copied $($package.Namespace)/$($package.Name)@$($package.Version)"
}

Write-Host "Synchronized $($packages.Count) packages to '$resolvedCacheRoot'" -ForegroundColor Green
