#pragma once

#include "Target/Target.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Rux {
// Format constants
namespace RcuSecType {
constexpr uint32_t Null = 0;
constexpr uint32_t Text = 1;
constexpr uint32_t Data = 2;
constexpr uint32_t RoData = 3;
constexpr uint32_t Bss = 4;
constexpr uint32_t Meta = 5;
} // namespace RcuSecType

namespace RcuSecFlag {
constexpr uint32_t Alloc = 0x01;
constexpr uint32_t Exec = 0x02;
constexpr uint32_t Read = 0x04;
constexpr uint32_t Write = 0x08;
constexpr uint32_t Merge = 0x10;
constexpr uint32_t Strings = 0x20;
} // namespace RcuSecFlag

namespace RcuSymKind {
constexpr uint8_t Unknown = 0;
constexpr uint8_t Func = 1;
constexpr uint8_t Data = 2;
constexpr uint8_t Const = 3;
constexpr uint8_t Section = 4;
constexpr uint8_t File = 5;
constexpr uint8_t ExternFunc = 6;
constexpr uint8_t ExternData = 7;
} // namespace RcuSymKind

namespace RcuSymVis {
constexpr uint8_t Local = 0;
constexpr uint8_t Global = 1;
constexpr uint8_t Weak = 2;
} // namespace RcuSymVis

// Machine architecture an object was compiled for, stored in byte 6 of the
// file header. An object is only meaningful to a linker targeting the same
// architecture.
namespace RcuArch {
constexpr uint8_t Unknown = 0x00;
constexpr uint8_t X86_64 = 0x01;
constexpr uint8_t AArch64 = 0x02;
} // namespace RcuArch

// Relocation kinds. Values below 16 patch a whole little-endian field and are
// architecture-neutral; 16 and above name an AArch64 instruction encoding,
// whose immediates are split across non-contiguous bit ranges and therefore
// cannot be expressed as a plain field width.
namespace RcuRelType {
constexpr uint16_t None = 0;
constexpr uint16_t Abs64 = 1;
constexpr uint16_t Abs32 = 2;
constexpr uint16_t Rel32 = 3;

constexpr uint16_t AArch64Call26 = 16;
constexpr uint16_t AArch64Jump26 = 17;
constexpr uint16_t AArch64CondBr19 = 18;
constexpr uint16_t AArch64TstBr14 = 19;
constexpr uint16_t AArch64AdrPrelPgHi21 = 20;
constexpr uint16_t AArch64AddAbsLo12Nc = 21;
constexpr uint16_t AArch64LdstAbsLo12Nc = 22;
constexpr uint16_t AArch64MovwUabsG0 = 23;
constexpr uint16_t AArch64MovwUabsG1 = 24;
constexpr uint16_t AArch64MovwUabsG2 = 25;
constexpr uint16_t AArch64MovwUabsG3 = 26;
constexpr uint16_t AArch64Prel32 = 27;
constexpr uint16_t AArch64Prel64 = 28;
} // namespace RcuRelType

// The architecture byte an object built for `arch` carries, or
// RcuArch::Unknown when RCU has no encoding for that architecture.
[[nodiscard]] constexpr uint8_t RcuArchFor(const Target::Arch arch) noexcept {
    switch (arch) {
    case Target::Arch::X86_64:
        return RcuArch::X86_64;
    case Target::Arch::AArch64:
        return RcuArch::AArch64;
    default:
        return RcuArch::Unknown;
    }
}

// Display spelling of an architecture byte, matching Target::ToDisplayString.
[[nodiscard]] constexpr std::string_view RcuArchName(const uint8_t arch) noexcept {
    switch (arch) {
    case RcuArch::X86_64:
        return "x86-64";
    case RcuArch::AArch64:
        return "AArch64";
    default:
        return "unknown";
    }
}

// Relocation name used by dumps and diagnostics. AArch64 kinds keep their ELF
// R_AARCH64_* spelling so a dump can be checked against the ABI document.
[[nodiscard]] constexpr std::string_view RcuRelTypeName(const uint16_t type) noexcept {
    switch (type) {
    case RcuRelType::None:
        return "NONE";
    case RcuRelType::Abs64:
        return "ABS_64";
    case RcuRelType::Abs32:
        return "ABS_32";
    case RcuRelType::Rel32:
        return "REL_32";
    case RcuRelType::AArch64Call26:
        return "AARCH64_CALL26";
    case RcuRelType::AArch64Jump26:
        return "AARCH64_JUMP26";
    case RcuRelType::AArch64CondBr19:
        return "AARCH64_CONDBR19";
    case RcuRelType::AArch64TstBr14:
        return "AARCH64_TSTBR14";
    case RcuRelType::AArch64AdrPrelPgHi21:
        return "AARCH64_ADR_PREL_PG_HI21";
    case RcuRelType::AArch64AddAbsLo12Nc:
        return "AARCH64_ADD_ABS_LO12_NC";
    case RcuRelType::AArch64LdstAbsLo12Nc:
        return "AARCH64_LDST_ABS_LO12_NC";
    case RcuRelType::AArch64MovwUabsG0:
        return "AARCH64_MOVW_UABS_G0";
    case RcuRelType::AArch64MovwUabsG1:
        return "AARCH64_MOVW_UABS_G1";
    case RcuRelType::AArch64MovwUabsG2:
        return "AARCH64_MOVW_UABS_G2";
    case RcuRelType::AArch64MovwUabsG3:
        return "AARCH64_MOVW_UABS_G3";
    case RcuRelType::AArch64Prel32:
        return "AARCH64_PREL32";
    case RcuRelType::AArch64Prel64:
        return "AARCH64_PREL64";
    default:
        return "?";
    }
}

constexpr uint16_t RCU_SEC_EXTERNAL = 0xFFFF;
constexpr uint16_t RCU_SEC_ABSOLUTE = 0xFFFE;

// Fixed section indices used by the generator
constexpr uint16_t RCU_TEXT_IDX = 0;
constexpr uint16_t RCU_RODATA_IDX = 1;
constexpr uint16_t RCU_DATA_IDX = 2;
constexpr uint16_t RCU_BSS_IDX = 3;

// In-memory structures

struct RcuReloc {
    uint32_t sectionOffset = 0;
    uint32_t symbolIndex = 0;
    uint16_t type = RcuRelType::None;
    int32_t addend = 0;
};

struct RcuSection {
    std::string name;
    uint32_t type = RcuSecType::Null;
    uint32_t flags = 0;
    uint16_t alignment = 1;
    std::vector<uint8_t> data;
    std::vector<RcuReloc> relocs;
};

struct RcuSymbol {
    std::string name;
    std::string typeName;
    uint32_t value = 0;
    uint32_t size = 0;
    uint16_t sectionIdx = RCU_SEC_EXTERNAL;
    uint8_t kind = RcuSymKind::Unknown;
    uint8_t visibility = RcuSymVis::Local;
};

struct RcuFile {
    uint8_t arch = RcuArch::X86_64;
    uint8_t flags = 0x00;
    bool hasMetadata = false;
    std::string sourcePath;
    std::string packageName;
    uint64_t buildTimestamp = 0;
    uint32_t ruxVersion = 0;
    uint32_t compilerFlags = 0;
    std::array<uint8_t, 32> sourceHash = {};
    std::vector<RcuSection> sections;
    std::vector<RcuSymbol> symbols;
};
} // namespace Rux
