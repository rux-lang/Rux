#pragma once

// Target-owned ELF64 policy consumed by the linker. Cross-linking therefore
// depends on the requested target, never on the host running the compiler.

#include "Target/Target.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace Rux::Target {
enum class ElfPltInstructionShape : std::uint8_t {
    X86_64,
    AArch64,
};

struct ElfDynamicRelocations {
    std::uint32_t jumpSlot;
    std::uint32_t globDat;
    std::uint32_t relative;
};

struct Elf64Profile {
    std::string_view targetName;
    OS os;
    Arch arch;
    std::uint8_t osAbi;
    std::uint16_t machine;
    std::string_view interpreter;
    std::string_view defaultLibc;
    std::uint64_t maximumLoadAlignment;
    std::uint64_t executableImageBase;
    std::uint64_t sharedImageBase;
    std::uint32_t freestandingExitSyscall;
    ElfDynamicRelocations dynamicRelocations;
    bool definesBsdProcessGlobals;
    ElfPltInstructionShape pltInstructionShape;
    std::size_t pltHeaderSize;
    std::size_t pltEntrySize;
};

// FreeBSD AArch64 is available here for direct-linker development while its
// public compiler-driver gate remains closed.
[[nodiscard]] constexpr std::optional<Elf64Profile> Elf64ProfileFor(const OS os, const Arch arch) noexcept {
    if (arch != Arch::X86_64 && arch != Arch::AArch64) {
        return std::nullopt;
    }

    const bool aarch64 = arch == Arch::AArch64;
    Elf64Profile profile{"",
                         os,
                         arch,
                         static_cast<std::uint8_t>(0),
                         static_cast<std::uint16_t>(aarch64 ? 0xB7 : 0x3E),
                         aarch64 ? "/lib/ld-linux-aarch64.so.1" : "/lib64/ld-linux-x86-64.so.2",
                         "libc.so.6",
                         aarch64 ? 0x10000U : 0x1000U,
                         0x400000,
                         0,
                         aarch64 ? 93U : 60U,
                         aarch64 ? ElfDynamicRelocations{1026, 1025, 1027} : ElfDynamicRelocations{7, 6, 8},
                         false,
                         aarch64 ? ElfPltInstructionShape::AArch64 : ElfPltInstructionShape::X86_64,
                         aarch64 ? 32U : 16U,
                         16};

    switch (os) {
    case OS::Linux:
        profile.targetName = aarch64 ? "linux-aarch64" : "linux-x86_64";
        return profile;
    case OS::Android:
        profile.targetName = aarch64 ? "android-aarch64" : "android-x86_64";
        return profile;
    case OS::FreeBSD:
        profile.targetName = aarch64 ? "freebsd-aarch64" : "freebsd-x86_64";
        profile.osAbi = 9;
        profile.interpreter = "/libexec/ld-elf.so.1";
        profile.defaultLibc = "libc.so.7";
        profile.freestandingExitSyscall = 1;
        profile.definesBsdProcessGlobals = true;
        return profile;
    case OS::DragonFlyBSD:
        profile.targetName = "dragonfly-x86_64";
        profile.interpreter = "/libexec/ld-elf.so.2";
        profile.defaultLibc = "libc.so.8";
        break;
    case OS::OpenBSD:
        profile.targetName = "openbsd-x86_64";
        profile.osAbi = 12;
        profile.interpreter = "/usr/libexec/ld.so";
        profile.defaultLibc = "libc.so.103.0";
        break;
    case OS::NetBSD:
        profile.targetName = "netbsd-x86_64";
        profile.osAbi = 2;
        profile.interpreter = "/libexec/ld.elf_so";
        profile.defaultLibc = "libc.so.12";
        break;
    case OS::Solaris:
    case OS::Illumos:
        profile.targetName = os == OS::Solaris ? "solaris-x86_64" : "illumos-x86_64";
        profile.osAbi = 6;
        profile.interpreter = "/lib/amd64/ld.so.1";
        profile.defaultLibc = "libc.so.1";
        break;
    default:
        return std::nullopt;
    }

    if (aarch64) {
        return std::nullopt;
    }
    profile.freestandingExitSyscall = 1;
    profile.definesBsdProcessGlobals = os == OS::DragonFlyBSD || os == OS::OpenBSD || os == OS::NetBSD;
    return profile;
}
} // namespace Rux::Target
