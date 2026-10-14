#pragma once

#include "Target/Target.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Rux {
// Format constants
namespace RcuFileFlag {
constexpr uint8_t HasMetadata = 0x01;
} // namespace RcuFileFlag

namespace RcuSecType {
constexpr uint32_t Null = 0;
constexpr uint32_t Text = 1;
constexpr uint32_t Data = 2;
constexpr uint32_t RoData = 3;
constexpr uint32_t Bss = 4;
constexpr uint32_t Meta = 5;
/// The thread-local template: every thread's copy of the thread-local variables starts as these bytes.
constexpr uint32_t Tls = 6;
} // namespace RcuSecType

namespace RcuSecFlag {
constexpr uint32_t Alloc = 0x01;
constexpr uint32_t Exec = 0x02;
constexpr uint32_t Read = 0x04;
constexpr uint32_t Write = 0x08;
constexpr uint32_t Merge = 0x10;
constexpr uint32_t Strings = 0x20;
constexpr uint32_t Tls = 0x40;
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

// Symbol binding. Global and Weak definitions are linkable across the objects of one artifact; Exported is a Global
// definition that a library artifact also publishes as part of its interface.
namespace RcuSymVis {
constexpr uint8_t Local = 0;
constexpr uint8_t Global = 1;
constexpr uint8_t Weak = 2;
constexpr uint8_t Exported = 3;

/// Whether a symbol with this visibility binds strongly across objects.
[[nodiscard]] constexpr bool IsGlobal(const uint8_t visibility) noexcept {
    return visibility == Global || visibility == Exported;
}

/// The visibility of a definition: object-local, linkable across objects, or also exported from a library.
[[nodiscard]] constexpr uint8_t ForDefinition(const bool isPublic, const bool isExported) noexcept {
    return isExported ? Exported : isPublic ? Global : Local;
}
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
/// A 32-bit field holding a thread-local variable's offset in the form its target reaches the variable with: from the
/// thread pointer on ELF, and from the start of the thread's block (`SECREL`) on PE.
constexpr uint16_t TlsOffset32 = 4;
/// A 32-bit PC-relative field addressing a thread-local variable's Mach-O descriptor rather than the variable itself.
/// The linker makes one descriptor per variable, so the relocation names the variable and the linker finds its
/// descriptor.
constexpr uint16_t TlvDescriptorRel32 = 5;

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
/// The high and low twelve bits of a thread-local offset, in the immediate of an `add`: R_AARCH64_TLSLE_ADD_TPREL_HI12
/// and _LO12_NC on ELF, and the section-relative pair on PE.
constexpr uint16_t AArch64TlsHi12 = 29;
constexpr uint16_t AArch64TlsLo12Nc = 30;
/// The page and page offset of a thread-local variable's Mach-O descriptor, in an `adrp` and an `add` that build its
/// address; ARM64_RELOC_TLVP_LOAD_PAGE21 and _PAGEOFF12 name the same fields.
constexpr uint16_t AArch64TlvDescriptorPage21 = 31;
constexpr uint16_t AArch64TlvDescriptorPageOff12 = 32;
} // namespace RcuRelType

/// The architecture byte an object built for `arch` carries, or RcuArch::Unknown when RCU has no encoding for that
/// architecture.
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

/// Display spelling of an architecture byte, matching Target::ToDisplayString.
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

/// Relocation name used by dumps and diagnostics. AArch64 kinds keep their ELF R_AARCH64_* spelling so a dump can be
/// checked against the ABI document.
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
    case RcuRelType::TlsOffset32:
        return "TLS_OFFSET_32";
    case RcuRelType::TlvDescriptorRel32:
        return "TLV_DESCRIPTOR_REL_32";
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
    case RcuRelType::AArch64TlsHi12:
        return "AARCH64_TLSLE_ADD_TPREL_HI12";
    case RcuRelType::AArch64TlsLo12Nc:
        return "AARCH64_TLSLE_ADD_TPREL_LO12_NC";
    case RcuRelType::AArch64TlvDescriptorPage21:
        return "AARCH64_TLVP_PAGE21";
    case RcuRelType::AArch64TlvDescriptorPageOff12:
        return "AARCH64_TLVP_PAGEOFF12";
    default:
        return "?";
    }
}

/// The word an ELF executable's entry stub stores the kernel's initial stack address in. The linker defines it for
/// every ELF executable, and `Core::ProcessStartFrame` reads it.
constexpr std::string_view kProcessStartFrameSymbol = "__rux_start_frame";

/// How the stack-trace printer's table of one module's functions is named. The linker treats what such a table refers
/// to as weak: listing a function there does not keep it in the image.
constexpr std::string_view kFunctionTablePrefix = "__rux_function_table_";

/// The word a PE image's thread-local storage directory names, which the loader fills with the image's TLS slot. The
/// linker defines it for every PE image with thread-local variables.
constexpr std::string_view kTlsIndexSymbol = "_tls_index";

/// Whether a relocation is an AArch64 instruction-field kind, whose immediate is split across an instruction word.
[[nodiscard]] constexpr bool IsAArch64InstructionRelocationType(const uint16_t type) noexcept {
    return (type >= RcuRelType::AArch64Call26 && type <= RcuRelType::AArch64MovwUabsG3) ||
           (type >= RcuRelType::AArch64TlsHi12 && type <= RcuRelType::AArch64TlvDescriptorPageOff12);
}

constexpr uint16_t RCU_SEC_EXTERNAL = 0xFFFF;
constexpr uint16_t RCU_SEC_ABSOLUTE = 0xFFFE;

// Fixed section indices used by the generator
constexpr uint16_t RCU_TEXT_IDX = 0;
constexpr uint16_t RCU_RODATA_IDX = 1;
constexpr uint16_t RCU_DATA_IDX = 2;
constexpr uint16_t RCU_BSS_IDX = 3;
constexpr uint16_t RCU_TLS_IDX = 4;

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
