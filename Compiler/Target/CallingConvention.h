#pragma once

#include "Target/Target.h"

#include <string_view>

namespace Rux {
// Language-level calling convention carried from syntax through codegen.
// Concrete register and stack rules are resolved by TargetInfo.
enum class CallingConvention {
    Default,
    C,       // #Abi(.C): whatever the target's C ABI is
    Win64,   // Microsoft x64 (rcx, rdx, r8, r9 + 32-byte shadow space)
    SysV,    // System V AMD64 (rdi, rsi, rdx, rcx, r8, r9)
    AAPCS64, // AArch64 procedure call standard (x0-x7 and v0-v7)
};

// The C ABI of a target. AArch64 uses AAPCS64 on every currently modeled OS;
// x86-64 uses Win64 on Windows and System V everywhere else.
constexpr CallingConvention PlatformCConvention(const Target::OS os, const Target::Arch arch) {
    if (arch == Target::Arch::AArch64) {
        return CallingConvention::AAPCS64;
    }
    return os == Target::OS::Windows ? CallingConvention::Win64 : CallingConvention::SysV;
}

// Rux functions use AAPCS64 on AArch64 and retain the existing x86-64 defaults:
// System V on Linux, Win64 on other systems.
constexpr CallingConvention PlatformDefaultConvention(const Target::OS os, const Target::Arch arch) {
    if (arch == Target::Arch::AArch64) {
        return CallingConvention::AAPCS64;
    }
    return os == Target::OS::Linux ? CallingConvention::SysV : CallingConvention::Win64;
}

// Host-defaulted wrappers, correct only when the target is the host. Anything
// on a compilation path that can cross-compile must pass the target OS and
// architecture from its TargetContext instead.
constexpr CallingConvention PlatformCConvention() {
    return PlatformCConvention(Target::HostOS, Target::HostArch);
}

constexpr CallingConvention PlatformDefaultConvention() {
    return PlatformDefaultConvention(Target::HostOS, Target::HostArch);
}

// Collapses `.C` to the concrete convention it stands for; every other value is
// already concrete. Resolving `Default` is left to the caller, because its
// meaning depends on whether the declaration is extern (the C ABI) or a Rux
// function (the internal ABI).
constexpr CallingConvention ResolveCConvention(const CallingConvention c, const Target::OS os,
                                               const Target::Arch arch) {
    return c == CallingConvention::C ? PlatformCConvention(os, arch) : c;
}

constexpr CallingConvention ResolveCConvention(const CallingConvention c) {
    return ResolveCConvention(c, Target::HostOS, Target::HostArch);
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

// Internal spelling used by IR dumps after target-dependent conventions have
// been resolved. AAPCS64 intentionally has no source-level `#Abi` spelling.
constexpr std::string_view ResolvedConventionName(const CallingConvention c) {
    switch (c) {
    case CallingConvention::Default:
        return "default";
    case CallingConvention::C:
        return "c";
    case CallingConvention::Win64:
        return "win64";
    case CallingConvention::SysV:
        return "sysv";
    case CallingConvention::AAPCS64:
        return "aapcs64";
    }
    return "unknown";
}
} // namespace Rux
