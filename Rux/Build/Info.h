#pragma once

namespace Rux
{
    #define BUILD_BRANCH   "dev"
    #define BUILD_HASH     "62f2146"
    #define BUILD_DATETIME "2025-11-09 21:00:39"
    #define BUILD_VERSION  "0.1.0"
#ifdef _DEBUG
    #define BUILD_PROFILE "Debug"
#else
    #define BUILD_PROFILE "Release"
#endif
}
