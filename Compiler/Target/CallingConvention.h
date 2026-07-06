#pragma once

#include "Target/Platform.h"

namespace Rux {

// Language-level calling convention carried from syntax through codegen.
// Concrete register and stack rules are resolved by TargetInfo.
enum class CallingConvention {
    Default,
    Win64, // Microsoft x64 (rcx, rdx, r8, r9 + 32-byte shadow space)
    SysV,  // System V AMD64 (rdi, rsi, rdx, rcx, r8, r9)
};

// The C ABI of the platform the compiler targets (the host): Win64 on Windows,
// System V AMD64 on every other x86-64 OS.
constexpr CallingConvention PlatformCConvention() {
#if RUX_OS_WINDOWS
    return CallingConvention::Win64;
#else
    return CallingConvention::SysV;
#endif
}

// Rux functions use the native ABI on Linux. Other targets retain their
// existing internal ABI until their entry points and compatibility thunks are
// migrated as a unit.
constexpr CallingConvention PlatformDefaultConvention() {
#if RUX_OS_LINUX
    return CallingConvention::SysV;
#else
    return CallingConvention::Win64;
#endif
}

} // namespace Rux
