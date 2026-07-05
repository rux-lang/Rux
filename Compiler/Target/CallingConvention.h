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

// The C ABI of the platform the compiler targets (the host): Win64 on
// Windows, System V AMD64 on every other x86-64 OS. Used to resolve the
// convention of extern C functions declared without an explicit @[Call(...)],
// so calls into libc.so.6 and friends pass arguments in the right registers.
// Rux's own internal calls keep the uniform Win64 convention regardless.
constexpr CallingConvention PlatformCConvention() {
#if RUX_OS_WINDOWS
    return CallingConvention::Win64;
#else
    return CallingConvention::SysV;
#endif
}

} // namespace Rux
