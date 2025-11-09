# PreBuild.ps1
$branch  = (git rev-parse --abbrev-ref HEAD)
$commit  = (git rev-parse --short HEAD)
$date    = (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')
$version = "0.1.0"

$content = @"
#pragma once

namespace Rux
{
    #define BUILD_BRANCH   "$branch"
    #define BUILD_HASH     "$commit"
    #define BUILD_DATETIME "$date"
    #define BUILD_VERSION  "$version"
#ifdef _DEBUG
    #define BUILD_PROFILE "Debug"
#else
    #define BUILD_PROFILE "Release"
#endif
}
"@

Set-Content -Path "$PSScriptRoot\Info.h" -Value $content
