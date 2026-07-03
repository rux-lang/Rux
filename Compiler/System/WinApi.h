#pragma once

// Single, consistent entry point for the Win32 API.
//
// Include this instead of <windows.h> directly. It applies the lean
// configuration (WIN32_LEAN_AND_MEAN, NOMINMAX) exactly once and isolates the
// include so clang-format never reorders <windows.h> relative to headers that
// must follow it (e.g. <psapi.h>, <winhttp.h>). On non-Windows hosts this
// header expands to nothing, so it is safe to include unconditionally.

#include "Target/Platform.h"

#if RUX_OS_WINDOWS
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif
