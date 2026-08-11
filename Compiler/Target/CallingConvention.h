#pragma once

#include "Target/Target.h"

#include <string_view>

namespace Rux {
// Language-level calling convention carried from syntax through codegen.
// Concrete register and stack rules are resolved by TargetInfo.
enum class CallingConvention {
    Default,
    C,     // #Abi(.C): whatever the target's C ABI is
    Win64, // Microsoft x64 (rcx, rdx, r8, r9 + 32-byte shadow space)
    SysV,  // System V AMD64 (rdi, rsi, rdx, rcx, r8, r9)
};

// The C ABI of a target OS: Win64 on Windows, System V AMD64 on every other
// x86-64 OS.
constexpr CallingConvention PlatformCConvention(const Target::OS os) {
    return os == Target::OS::Windows ? CallingConvention::Win64 : CallingConvention::SysV;
}

// Rux functions use the native ABI on Linux. Other targets retain their
// existing internal ABI; explicit `.C` declarations use the platform C ABI.
constexpr CallingConvention PlatformDefaultConvention(const Target::OS os) {
    return os == Target::OS::Linux ? CallingConvention::SysV : CallingConvention::Win64;
}

// Host-defaulted wrappers, correct only when the target is the host. Anything
// on a compilation path that can cross-compile must pass the target OS from its
// TargetContext instead, or a Linux-to-Windows build picks the host's ABI.
constexpr CallingConvention PlatformCConvention() {
    return PlatformCConvention(Target::HostOS);
}

constexpr CallingConvention PlatformDefaultConvention() {
    return PlatformDefaultConvention(Target::HostOS);
}

// Collapses `.C` to the concrete convention it stands for; every other value is
// already concrete. Resolving `Default` is left to the caller, because its
// meaning depends on whether the declaration is extern (the C ABI) or a Rux
// function (the internal ABI).
constexpr CallingConvention ResolveCConvention(const CallingConvention c, const Target::OS os) {
    return c == CallingConvention::C ? PlatformCConvention(os) : c;
}

constexpr CallingConvention ResolveCConvention(const CallingConvention c) {
    return ResolveCConvention(c, Target::HostOS);
}

// The `#Abi(...)` variant a convention was written as, for dumps. Empty for
// Default, which is spelled by leaving the attribute out.
constexpr std::string_view ConventionName(const CallingConvention c) {
    switch (c) {
    case CallingConvention::C:
        return ".C";
    case CallingConvention::Win64:
        return ".Win64";
    case CallingConvention::SysV:
        return ".SysV";
    default:
        return "";
    }
}
} // namespace Rux
