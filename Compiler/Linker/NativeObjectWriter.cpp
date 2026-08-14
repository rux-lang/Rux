#include "Linker/NativeObjectWriter.h"

#include "Linker/AArch64Relocation.h"
#include "Linker/Coff/CoffObjectWriter.h"
#include "Linker/LinkerInternal.h"
#include "Target/ElfProfile.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace Rux {
namespace {
class Bytes {
public:
    std::vector<std::uint8_t> data;

    void U8(const std::uint8_t value) {
        data.push_back(value);
    }

    void U16(const std::uint16_t value) {
        U8(static_cast<std::uint8_t>(value));
        U8(static_cast<std::uint8_t>(value >> 8U));
    }

    void U32(const std::uint32_t value) {
        U16(static_cast<std::uint16_t>(value));
        U16(static_cast<std::uint16_t>(value >> 16U));
    }

    void U64(const std::uint64_t value) {
        U32(static_cast<std::uint32_t>(value));
        U32(static_cast<std::uint32_t>(value >> 32U));
    }

    void I64(const std::int64_t value) {
        U64(static_cast<std::uint64_t>(value));
    }

    void Raw(const std::span<const std::uint8_t> bytes) {
        data.insert(data.end(), bytes.begin(), bytes.end());
    }

    void Zeros(const std::size_t count) {
        data.resize(data.size() + count);
    }

    void Fixed(const std::string_view text, const std::size_t count) {
        const auto copied = std::min(text.size(), count);
        data.insert(data.end(), text.begin(), text.begin() + static_cast<std::ptrdiff_t>(copied));
        Zeros(count - copied);
    }

    void Align(const std::size_t alignment) {
        while (data.size() % alignment != 0) {
            U8(0);
        }
    }

    void Patch32(const std::size_t offset, const std::uint32_t value) {
        for (std::size_t i = 0; i < 4; ++i) {
            data[offset + i] = static_cast<std::uint8_t>(value >> (i * 8U));
        }
    }

    void Patch64(const std::size_t offset, const std::uint64_t value) {
        for (std::size_t i = 0; i < 8; ++i) {
            data[offset + i] = static_cast<std::uint8_t>(value >> (i * 8U));
        }
    }
};

std::uint32_t AddString(std::vector<std::uint8_t> &table, const std::string_view value) {
    const auto offset = static_cast<std::uint32_t>(table.size());
    table.insert(table.end(), value.begin(), value.end());
    table.push_back(0);
    return offset;
}

// Mach-O cputype/cpusubtype: CPU_TYPE_X86_64 with CPU_SUBTYPE_X86_64_ALL, and
// CPU_TYPE_ARM64 with CPU_SUBTYPE_ARM64_ALL.
std::pair<std::uint32_t, std::uint32_t> MachOCpuType(const Target::Arch targetArch) noexcept {
    switch (targetArch) {
    case Target::Arch::X86_64:
        return {0x0100'0007, 3};
    case Target::Arch::AArch64:
        return {0x0100'000C, 0};
    default:
        return {0, 0};
    }
}

// The architecture-neutral relocation kinds, which name a whole little-endian
// field. Every container has a number for them; architecture-specific mappings
// decide which AArch64 instruction fields their format can carry.
bool IsFieldRelocation(const std::uint16_t type) noexcept {
    return type == RcuRelType::None || type == RcuRelType::Abs64 || type == RcuRelType::Abs32 ||
           type == RcuRelType::Rel32;
}

bool IsAArch64MachORelocation(const std::uint16_t type) noexcept {
    switch (type) {
    case RcuRelType::None:
    case RcuRelType::Abs64:
    case RcuRelType::Abs32:
    case RcuRelType::AArch64Call26:
    case RcuRelType::AArch64Jump26:
    case RcuRelType::AArch64AdrPrelPgHi21:
    case RcuRelType::AArch64AddAbsLo12Nc:
    case RcuRelType::AArch64LdstAbsLo12Nc:
        return true;
    default:
        return false;
    }
}

// R_AARCH64_LDST<n>_ABS_LO12_NC, one number per access width, because a linker
// placing the symbol's low 12 bits into the immediate has to scale them down by
// the width the instruction moves. The 8-bit form is numbered next to
// ADD_ABS_LO12_NC and the wider ones sit together well after it, with the
// 128-bit vector form later still.
std::uint32_t ElfAArch64LdstRelocationType(const std::uint32_t word) noexcept {
    switch (AArch64LoadStoreScale(word)) {
    case 0:
        return 278; // R_AARCH64_LDST8_ABS_LO12_NC
    case 1:
        return 284; // R_AARCH64_LDST16_ABS_LO12_NC
    case 2:
        return 285; // R_AARCH64_LDST32_ABS_LO12_NC
    case 3:
        return 286; // R_AARCH64_LDST64_ABS_LO12_NC
    default:
        return 299; // R_AARCH64_LDST128_ABS_LO12_NC
    }
}

// The ELF relocation number a kind takes for `targetArch`, or nullopt when that
// architecture has none for it. `word` is the four bytes the relocation sits
// on, which only the load-store form reads.
//
// The MOVW kinds map to the _NC forms deliberately: the four of them together
// carry one 64-bit value, a halfword each, so a checking G0 would reject every
// address above 65535. G3 has no _NC spelling because it holds the top
// halfword and there is nothing left over for it to drop.
std::optional<std::uint32_t> ElfRelocationType(const std::uint16_t type, const Target::Arch targetArch,
                                               const std::uint32_t word) noexcept {
    if (targetArch == Target::Arch::X86_64) {
        switch (type) {
        case RcuRelType::None:
            return 0; // R_X86_64_NONE
        case RcuRelType::Abs64:
            return 1; // R_X86_64_64
        case RcuRelType::Abs32:
            return 10; // R_X86_64_32
        case RcuRelType::Rel32:
            return 2; // R_X86_64_PC32
        default:
            return std::nullopt;
        }
    }
    switch (type) {
    case RcuRelType::None:
        return 0; // R_AARCH64_NONE
    case RcuRelType::Abs64:
        return 257; // R_AARCH64_ABS64
    case RcuRelType::Abs32:
        return 258; // R_AARCH64_ABS32
    case RcuRelType::AArch64Prel64:
        return 260; // R_AARCH64_PREL64
    case RcuRelType::Rel32:
    case RcuRelType::AArch64Prel32:
        return 261; // R_AARCH64_PREL32
    case RcuRelType::AArch64MovwUabsG0:
        return 264; // R_AARCH64_MOVW_UABS_G0_NC
    case RcuRelType::AArch64MovwUabsG1:
        return 266; // R_AARCH64_MOVW_UABS_G1_NC
    case RcuRelType::AArch64MovwUabsG2:
        return 268; // R_AARCH64_MOVW_UABS_G2_NC
    case RcuRelType::AArch64MovwUabsG3:
        return 269; // R_AARCH64_MOVW_UABS_G3
    case RcuRelType::AArch64AdrPrelPgHi21:
        return 275; // R_AARCH64_ADR_PREL_PG_HI21
    case RcuRelType::AArch64AddAbsLo12Nc:
        return 277; // R_AARCH64_ADD_ABS_LO12_NC
    case RcuRelType::AArch64TstBr14:
        return 279; // R_AARCH64_TSTBR14
    case RcuRelType::AArch64CondBr19:
        return 280; // R_AARCH64_CONDBR19
    case RcuRelType::AArch64Jump26:
        return 282; // R_AARCH64_JUMP26
    case RcuRelType::AArch64Call26:
        return 283; // R_AARCH64_CALL26
    case RcuRelType::AArch64LdstAbsLo12Nc:
        return ElfAArch64LdstRelocationType(word);
    default:
        return std::nullopt;
    }
}

// The instruction a relocation names, or zero when the site lies past the end
// of its section. Only the load-store relocation reads it, and the alternative
// to reading it would be a relocation kind per access width in RCU itself.
std::uint32_t InstructionAt(const std::vector<std::uint8_t> &data, const std::uint32_t offset) noexcept {
    if (static_cast<std::size_t>(offset) + 4 > data.size()) {
        return 0;
    }
    std::uint32_t word = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        word |= static_cast<std::uint32_t>(data[offset + i]) << (i * 8U);
    }
    return word;
}

// The addend an ELF RELA entry carries. R_X86_64_PC32 is measured from the end
// of the four-byte field it patches, so reaching the symbol takes a -4; every
// AArch64 relocation is measured from the start of the instruction or field it
// names and takes none.
std::int64_t ElfAddend(const RcuReloc &relocation, const Target::Arch targetArch) noexcept {
    const bool biased = targetArch == Target::Arch::X86_64 && relocation.type == RcuRelType::Rel32;
    return biased ? relocation.addend - 4 : relocation.addend;
}

std::vector<std::size_t> SymbolOrder(const RcuFile &file) {
    std::vector<std::size_t> order(file.symbols.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_partition(order.begin(), order.end(),
                          [&](const std::size_t index) { return file.symbols[index].visibility == RcuSymVis::Local; });
    return order;
}

void PatchInlineField(Buf &data, const RcuReloc &relocation, const std::int64_t value) {
    const std::size_t width = relocation.type == RcuRelType::Abs64 ? 8 : 4;
    if (static_cast<std::size_t>(relocation.sectionOffset) + width > data.size()) {
        return;
    }
    for (std::size_t i = 0; i < width; ++i) {
        data[relocation.sectionOffset + i] = static_cast<std::uint8_t>(static_cast<std::uint64_t>(value) >> (i * 8U));
    }
}

struct MachORelocation {
    std::uint32_t address = 0;
    std::uint32_t symbol = 0;
    std::uint8_t length = 0;
    std::uint8_t type = 0;
    bool pcRelative = false;
    bool external = false;
};

bool FitsSigned24(const std::int64_t value) noexcept {
    constexpr std::int64_t limit = std::int64_t{1} << 23;
    return value >= -limit && value < limit;
}

bool IsAArch64Branch26(const std::uint32_t word) noexcept {
    return (word & 0x7C00'0000U) == 0x1400'0000U;
}

bool IsAArch64Call26(const std::uint32_t word) noexcept {
    return (word & 0xFC00'0000U) == 0x9400'0000U;
}

bool IsAArch64Jump26(const std::uint32_t word) noexcept {
    return (word & 0xFC00'0000U) == 0x1400'0000U;
}

bool IsAArch64CondBranch19(const std::uint32_t word) noexcept {
    const bool condition = (word & 0xFF00'0010U) == 0x5400'0000U;
    const bool compare = (word & 0x7E00'0000U) == 0x3400'0000U;
    return condition || compare;
}

bool IsAArch64TestBranch14(const std::uint32_t word) noexcept {
    return (word & 0x7E00'0000U) == 0x3600'0000U;
}

bool IsAArch64Adrp(const std::uint32_t word) noexcept {
    return (word & 0x9F00'0000U) == 0x9000'0000U;
}

bool IsAArch64UnshiftedAddImmediate(const std::uint32_t word) noexcept {
    return (word & 0x7F80'0000U) == 0x1100'0000U && (word & 0x0040'0000U) == 0;
}

bool IsAArch64UnsignedOffsetLoadStore(const std::uint32_t word) noexcept {
    return (word & 0x3B00'0000U) == 0x3900'0000U;
}

bool IsAArch64MoveWide(const std::uint32_t word, const unsigned halfword) noexcept {
    const bool moveWide = (word & 0x1F80'0000U) == 0x1280'0000U;
    const bool validWidth = halfword < 2 || (word & 0x8000'0000U) != 0;
    return moveWide && validWidth && ((word >> 21U) & 3U) == halfword;
}

bool ValidateAArch64ElfRelocation(const RcuSection &section, const RcuReloc &relocation,
                                  const std::string_view symbolName, std::string &error) {
    const auto fail = [&](const std::string_view reason) {
        error = std::format("{} relocation against '{}' in section {} {}", RcuRelTypeName(relocation.type), symbolName,
                            section.name, reason);
        return false;
    };

    std::size_t fieldWidth = 0;
    switch (relocation.type) {
    case RcuRelType::None:
        return true;
    case RcuRelType::Abs64:
    case RcuRelType::AArch64Prel64:
        fieldWidth = 8;
        break;
    case RcuRelType::Abs32:
    case RcuRelType::Rel32:
    case RcuRelType::AArch64Prel32:
        fieldWidth = 4;
        break;
    default:
        break;
    }
    if (fieldWidth != 0) {
        return static_cast<std::size_t>(relocation.sectionOffset) + fieldWidth <= section.data.size()
                 ? true
                 : fail("extends beyond the section data");
    }

    if (relocation.sectionOffset % 4 != 0 ||
        static_cast<std::size_t>(relocation.sectionOffset) + 4 > section.data.size()) {
        return fail("does not name an aligned four-byte instruction");
    }
    const std::uint32_t word = InstructionAt(section.data, relocation.sectionOffset);
    const bool alignedAddend = relocation.addend % 4 == 0;
    switch (relocation.type) {
    case RcuRelType::AArch64Call26:
        return IsAArch64Call26(word) && alignedAddend ? true : fail("requires BL and a four-byte-aligned addend");
    case RcuRelType::AArch64Jump26:
        return IsAArch64Jump26(word) && alignedAddend ? true : fail("requires B and a four-byte-aligned addend");
    case RcuRelType::AArch64CondBr19:
        return IsAArch64CondBranch19(word) && alignedAddend
                 ? true
                 : fail("requires a conditional or compare-and-branch instruction and a four-byte-aligned addend");
    case RcuRelType::AArch64TstBr14:
        return IsAArch64TestBranch14(word) && alignedAddend
                 ? true
                 : fail("requires a test-and-branch instruction and a four-byte-aligned addend");
    case RcuRelType::AArch64AdrPrelPgHi21:
        return IsAArch64Adrp(word) ? true : fail("requires an ADRP instruction");
    case RcuRelType::AArch64AddAbsLo12Nc:
        return IsAArch64UnshiftedAddImmediate(word) ? true : fail("requires an unshifted ADD-immediate instruction");
    case RcuRelType::AArch64LdstAbsLo12Nc: {
        if (!IsAArch64UnsignedOffsetLoadStore(word)) {
            return fail("requires an unsigned-offset load or store instruction");
        }
        const unsigned scale = AArch64LoadStoreScale(word);
        const auto mask = (std::uint64_t{1} << scale) - 1U;
        return (static_cast<std::uint64_t>(relocation.addend) & mask) == 0
                 ? true
                 : fail("has an addend that is not aligned to the access width");
    }
    case RcuRelType::AArch64MovwUabsG0:
    case RcuRelType::AArch64MovwUabsG1:
    case RcuRelType::AArch64MovwUabsG2:
    case RcuRelType::AArch64MovwUabsG3: {
        const unsigned halfword = relocation.type - RcuRelType::AArch64MovwUabsG0;
        return IsAArch64MoveWide(word, halfword) ? true : fail("requires the matching MOVW halfword instruction");
    }
    default:
        return true;
    }
}

void ClearAArch64RelocationField(Buf &data, const RcuReloc &relocation, const std::uint32_t word) {
    std::uint32_t cleared = word;
    switch (relocation.type) {
    case RcuRelType::AArch64Call26:
    case RcuRelType::AArch64Jump26:
        cleared &= 0xFC00'0000U;
        break;
    case RcuRelType::AArch64AdrPrelPgHi21:
        cleared &= ~((0x3U << 29U) | (0x7FFFFU << 5U));
        break;
    case RcuRelType::AArch64AddAbsLo12Nc:
    case RcuRelType::AArch64LdstAbsLo12Nc:
        cleared &= ~(0xFFFU << 10U);
        break;
    default:
        break;
    }
    Patch32(data, relocation.sectionOffset, cleared);
}

bool BuildAArch64MachORelocations(const RcuFile &file, const std::size_t sectionIndex,
                                  const std::vector<std::uint32_t> &symbolRemap, Buf &data,
                                  std::vector<MachORelocation> &records, std::string &error) {
    const auto &section = file.sections[sectionIndex];
    for (const auto &relocation : section.relocs) {
        if (relocation.type == RcuRelType::None) {
            continue;
        }
        const auto &target = file.symbols[relocation.symbolIndex];
        const bool sectionRelative = target.visibility == RcuSymVis::Local && target.sectionIdx < file.sections.size();
        const std::uint32_t symbol =
            sectionRelative ? static_cast<std::uint32_t>(target.sectionIdx + 1) : symbolRemap[relocation.symbolIndex];
        const std::int64_t addend = static_cast<std::int64_t>(relocation.addend) +
                                    (sectionRelative ? static_cast<std::int64_t>(target.value) : 0);
        const auto fail = [&](const std::string_view reason) {
            error = std::format("{} relocation against '{}' in section {} {}", RcuRelTypeName(relocation.type),
                                target.name, section.name, reason);
            return false;
        };

        if (relocation.type == RcuRelType::Abs64 || relocation.type == RcuRelType::Abs32) {
            const std::size_t width = relocation.type == RcuRelType::Abs64 ? 8 : 4;
            if (static_cast<std::size_t>(relocation.sectionOffset) + width > data.size()) {
                return fail("extends beyond the section data");
            }
            PatchInlineField(data, relocation, addend);
            records.push_back({.address = relocation.sectionOffset,
                               .symbol = symbol,
                               .length = static_cast<std::uint8_t>(relocation.type == RcuRelType::Abs64 ? 3 : 2),
                               .type = 0,
                               .pcRelative = false,
                               .external = !sectionRelative});
            continue;
        }

        if (relocation.sectionOffset % 4 != 0 || static_cast<std::size_t>(relocation.sectionOffset) + 4 > data.size()) {
            return fail("does not name an aligned four-byte instruction");
        }
        const std::uint32_t word = InstructionAt(data, relocation.sectionOffset);
        std::uint8_t type = 0;
        bool pcRelative = false;
        switch (relocation.type) {
        case RcuRelType::AArch64Call26:
        case RcuRelType::AArch64Jump26:
            if (!IsAArch64Branch26(word)) {
                return fail("requires a B or BL instruction");
            }
            if (addend % 4 != 0) {
                return fail("has an addend that is not four-byte aligned");
            }
            type = 2; // ARM64_RELOC_BRANCH26
            pcRelative = true;
            break;
        case RcuRelType::AArch64AdrPrelPgHi21:
            if (!IsAArch64Adrp(word)) {
                return fail("requires an ADRP instruction");
            }
            type = 3; // ARM64_RELOC_PAGE21
            pcRelative = true;
            break;
        case RcuRelType::AArch64AddAbsLo12Nc:
            if (!IsAArch64UnshiftedAddImmediate(word)) {
                return fail("requires an unshifted ADD-immediate instruction");
            }
            type = 4; // ARM64_RELOC_PAGEOFF12
            break;
        case RcuRelType::AArch64LdstAbsLo12Nc: {
            if (!IsAArch64UnsignedOffsetLoadStore(word)) {
                return fail("requires an unsigned-offset load or store instruction");
            }
            const unsigned scale = AArch64LoadStoreScale(word);
            if ((static_cast<std::uint64_t>(addend) & ((std::uint64_t{1} << scale) - 1U)) != 0) {
                return fail("has an addend that is not aligned to the access width");
            }
            type = 4; // ARM64_RELOC_PAGEOFF12
            break;
        }
        default:
            return fail("is not supported by the Mach-O AArch64 object writer");
        }

        ClearAArch64RelocationField(data, relocation, word);
        if (addend != 0) {
            if (!FitsSigned24(addend)) {
                return fail("has an addend that does not fit in the signed 24-bit ARM64_RELOC_ADDEND field");
            }
            records.push_back({.address = relocation.sectionOffset,
                               .symbol = static_cast<std::uint32_t>(addend) & 0x00FF'FFFFU,
                               .length = 2,
                               .type = 10}); // ARM64_RELOC_ADDEND
        }
        records.push_back({.address = relocation.sectionOffset,
                           .symbol = symbol,
                           .length = 2,
                           .type = type,
                           .pcRelative = pcRelative,
                           .external = !sectionRelative});
    }
    return true;
}

struct ElfSection {
    std::string name;
    std::uint32_t type = 1;
    std::uint64_t flags = 0;
    std::uint64_t alignment = 1;
    std::uint64_t entrySize = 0;
    std::uint32_t link = 0;
    std::uint32_t info = 0;
    std::vector<std::uint8_t> data;
    std::uint64_t offset = 0;
};

bool WriteElf(const RcuFile &file, const Target::Elf64Profile &profile, NativeObject &output, std::string &error) {
    (void)error;
    const Target::Arch targetArch = profile.arch;
    // Section 0 is the reserved null entry, which is all zeros: a section index
    // of 0 means "none", so the header standing in for it must not describe a
    // section of its own.
    std::vector<ElfSection> sections(1);
    sections.front().type = 0;
    sections.front().alignment = 0;
    for (const auto &input : file.sections) {
        ElfSection section;
        section.name = input.name == ".rodata" ? ".rodata" : input.name;
        section.type = input.type == RcuSecType::Bss ? 8 : 1;
        section.flags = ((input.flags & RcuSecFlag::Write) != 0 ? 0x1U : 0U) |
                        ((input.flags & RcuSecFlag::Alloc) != 0 ? 0x2U : 0U) |
                        ((input.flags & RcuSecFlag::Exec) != 0 ? 0x4U : 0U);
        section.alignment = std::max<std::uint16_t>(input.alignment, 1);
        section.data = input.data;
        sections.push_back(std::move(section));
    }

    const auto order = SymbolOrder(file);
    std::vector<std::uint32_t> symbolRemap(file.symbols.size());
    std::vector<std::uint8_t> strings(1);
    Bytes symbols;
    symbols.Zeros(24);
    std::uint32_t localCount = 1;
    for (std::size_t i = 0; i < order.size(); ++i) {
        const auto oldIndex = order[i];
        const auto &symbol = file.symbols[oldIndex];
        symbolRemap[oldIndex] = static_cast<std::uint32_t>(i + 1);
        const auto name = AddString(strings, symbol.name);
        const std::uint8_t bind = symbol.visibility == RcuSymVis::Local ? 0
                                : symbol.visibility == RcuSymVis::Weak  ? 2
                                                                        : 1;
        if (bind == 0) {
            ++localCount;
        }
        const std::uint8_t type = symbol.kind == RcuSymKind::Func ? 2 : symbol.kind == RcuSymKind::Unknown ? 0 : 1;
        symbols.U32(name);
        symbols.U8(static_cast<std::uint8_t>((bind << 4U) | type));
        symbols.U8(0);
        const std::uint16_t sectionIndex = symbol.sectionIdx == RCU_SEC_EXTERNAL ? 0
                                         : symbol.sectionIdx == RCU_SEC_ABSOLUTE
                                             ? 0xfff1
                                             : static_cast<std::uint16_t>(symbol.sectionIdx + 1);
        symbols.U16(sectionIndex);
        symbols.U64(symbol.value);
        symbols.U64(symbol.size);
        if (bind != 0 && sectionIndex != 0) {
            output.publicSymbols.push_back(symbol.name);
        }
    }

    const std::size_t relocationStart = sections.size();
    for (std::size_t i = 0; i < file.sections.size(); ++i) {
        if (file.sections[i].relocs.empty()) {
            continue;
        }
        ElfSection relocationSection;
        relocationSection.name = ".rela" + file.sections[i].name;
        relocationSection.type = 4;
        // SHF_INFO_LINK: sh_info names the section these entries patch rather
        // than being a count, which is what a linker reads it as.
        relocationSection.flags = 0x40;
        relocationSection.alignment = 8;
        relocationSection.entrySize = 24;
        relocationSection.info = static_cast<std::uint32_t>(i + 1);
        Bytes relocations;
        for (const auto &relocation : file.sections[i].relocs) {
            const std::uint32_t type = ElfRelocationType(relocation.type, targetArch,
                                                         InstructionAt(file.sections[i].data, relocation.sectionOffset))
                                           .value_or(0);
            relocations.U64(relocation.sectionOffset);
            relocations.U64((static_cast<std::uint64_t>(symbolRemap[relocation.symbolIndex]) << 32U) | type);
            relocations.I64(ElfAddend(relocation, targetArch));
        }
        relocationSection.data = std::move(relocations.data);
        sections.push_back(std::move(relocationSection));
    }
    const auto symtabIndex = static_cast<std::uint32_t>(sections.size());
    const auto strtabIndex = symtabIndex + 1;
    for (std::size_t i = relocationStart; i < sections.size(); ++i) {
        sections[i].link = symtabIndex;
    }
    ElfSection symbolSection;
    symbolSection.name = ".symtab";
    symbolSection.type = 2;
    symbolSection.alignment = 8;
    symbolSection.entrySize = 24;
    symbolSection.link = strtabIndex;
    symbolSection.info = localCount;
    symbolSection.data = std::move(symbols.data);
    sections.push_back(std::move(symbolSection));
    ElfSection stringSection;
    stringSection.name = ".strtab";
    stringSection.type = 3;
    stringSection.data = std::move(strings);
    sections.push_back(std::move(stringSection));
    const auto shstrtabIndex = static_cast<std::uint16_t>(sections.size());
    ElfSection sectionNameSection;
    sectionNameSection.name = ".shstrtab";
    sectionNameSection.type = 3;
    sections.push_back(std::move(sectionNameSection));

    std::vector<std::uint8_t> sectionNames(1);
    std::vector<std::uint32_t> sectionNameOffsets;
    sectionNameOffsets.reserve(sections.size());
    for (const auto &section : sections) {
        sectionNameOffsets.push_back(section.name.empty() ? 0 : AddString(sectionNames, section.name));
    }
    sections.back().data = std::move(sectionNames);

    Bytes out;
    out.Zeros(64);
    for (std::size_t i = 1; i < sections.size(); ++i) {
        out.Align(static_cast<std::size_t>(sections[i].alignment));
        sections[i].offset = out.data.size();
        if (sections[i].type != 8) {
            out.Raw(sections[i].data);
        }
    }
    out.Align(8);
    const auto sectionHeaderOffset = out.data.size();
    for (std::size_t i = 0; i < sections.size(); ++i) {
        const auto &section = sections[i];
        out.U32(sectionNameOffsets[i]);
        out.U32(section.type);
        out.U64(section.flags);
        out.U64(0);
        out.U64(section.offset);
        out.U64(section.data.size());
        out.U32(section.link);
        out.U32(section.info);
        out.U64(section.alignment);
        out.U64(section.entrySize);
    }
    const std::array<std::uint8_t, 16> ident = {0x7f, 'E', 'L', 'F', 2, 1, 1, profile.osAbi, 0, 0, 0, 0, 0, 0, 0, 0};
    std::copy(ident.begin(), ident.end(), out.data.begin());
    out.Patch32(16, 1U | (static_cast<std::uint32_t>(profile.machine) << 16U));
    out.Patch32(20, 1);
    out.Patch64(24, 0);
    out.Patch64(32, 0);
    out.Patch64(40, sectionHeaderOffset);
    out.Patch32(48, 0);
    out.data[52] = 64;
    out.data[53] = 0;
    out.data[58] = 64;
    out.data[59] = 0;
    out.data[60] = static_cast<std::uint8_t>(sections.size());
    out.data[61] = static_cast<std::uint8_t>(sections.size() >> 8U);
    out.data[62] = static_cast<std::uint8_t>(shstrtabIndex);
    out.data[63] = static_cast<std::uint8_t>(shstrtabIndex >> 8U);
    output.bytes = std::move(out.data);
    return true;
}

bool WriteMachO(const RcuFile &file, const Target::Arch targetArch, NativeObject &output, std::string &error) {
    if (file.sections.size() > 255) {
        error = "too many sections for a Mach-O object";
        return false;
    }
    const std::size_t sectionCount = file.sections.size();
    const std::uint32_t segmentCommandSize = static_cast<std::uint32_t>(72 + sectionCount * 80);
    const std::uint32_t commandsSize = segmentCommandSize + 24;
    const std::size_t dataStart = 32 + commandsSize;
    const auto order = SymbolOrder(file);
    std::vector<std::uint32_t> symbolRemap(file.symbols.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        symbolRemap[order[i]] = static_cast<std::uint32_t>(i);
    }
    std::vector<std::vector<std::uint8_t>> sectionData;
    std::vector<std::vector<MachORelocation>> sectionRelocations(sectionCount);
    std::vector<std::uint32_t> sectionOffsets(sectionCount);
    std::vector<std::uint32_t> relocationOffsets(sectionCount);
    std::size_t cursor = dataStart;
    for (std::size_t i = 0; i < sectionCount; ++i) {
        sectionData.push_back(file.sections[i].data);
        if (targetArch == Target::Arch::AArch64) {
            if (!BuildAArch64MachORelocations(file, i, symbolRemap, sectionData.back(), sectionRelocations[i], error)) {
                return false;
            }
        }
        else {
            for (const auto &relocation : file.sections[i].relocs) {
                PatchInlineField(sectionData.back(), relocation, relocation.addend);
            }
        }
        sectionOffsets[i] = static_cast<std::uint32_t>(cursor);
        cursor += sectionData.back().size();
    }
    for (std::size_t i = 0; i < sectionCount; ++i) {
        const std::size_t count =
            targetArch == Target::Arch::AArch64 ? sectionRelocations[i].size() : file.sections[i].relocs.size();
        relocationOffsets[i] = count == 0 ? 0 : static_cast<std::uint32_t>(cursor);
        cursor += count * 8;
    }
    const auto symbolOffset = static_cast<std::uint32_t>(cursor);
    const auto stringOffset = symbolOffset + static_cast<std::uint32_t>(file.symbols.size() * 16);
    std::vector<std::uint8_t> strings(1);

    const auto [cpuType, cpuSubtype] = MachOCpuType(targetArch);
    Bytes out;
    out.U32(0xfeedfacf);
    out.U32(cpuType);
    out.U32(cpuSubtype);
    out.U32(1);
    out.U32(2);
    out.U32(commandsSize);
    out.U32(0);
    out.U32(0);
    out.U32(0x19);
    out.U32(segmentCommandSize);
    out.Zeros(16);
    out.U64(0);
    out.U64(0);
    out.U64(dataStart);
    out.U64(symbolOffset - dataStart);
    out.U32(7);
    out.U32(7);
    out.U32(static_cast<std::uint32_t>(sectionCount));
    out.U32(0);
    for (std::size_t i = 0; i < sectionCount; ++i) {
        const auto &section = file.sections[i];
        std::string_view name = section.type == RcuSecType::Text   ? "__text"
                              : section.type == RcuSecType::RoData ? "__const"
                                                                   : "__data";
        const std::string_view segment = section.type == RcuSecType::Data ? "__DATA" : "__TEXT";
        out.Fixed(name, 16);
        out.Fixed(segment, 16);
        out.U64(0);
        out.U64(sectionData[i].size());
        out.U32(sectionOffsets[i]);
        out.U32(
            static_cast<std::uint32_t>(std::countr_zero(std::bit_ceil(std::max<std::uint16_t>(1, section.alignment)))));
        out.U32(relocationOffsets[i]);
        out.U32(static_cast<std::uint32_t>(targetArch == Target::Arch::AArch64 ? sectionRelocations[i].size()
                                                                               : section.relocs.size()));
        out.U32(section.type == RcuSecType::Text ? 0x80000400U : 0);
        out.U32(0);
        out.U32(0);
        out.U32(0);
    }
    out.U32(0x2);
    out.U32(24);
    out.U32(symbolOffset);
    out.U32(static_cast<std::uint32_t>(file.symbols.size()));
    out.U32(stringOffset);
    const std::size_t stringSizePatch = out.data.size();
    out.U32(0);
    for (const auto &data : sectionData) {
        out.Raw(data);
    }
    for (std::size_t i = 0; i < sectionCount; ++i) {
        if (targetArch == Target::Arch::AArch64) {
            for (const auto &relocation : sectionRelocations[i]) {
                out.U32(relocation.address);
                out.U32(relocation.symbol | (static_cast<std::uint32_t>(relocation.pcRelative) << 24U) |
                        (static_cast<std::uint32_t>(relocation.length) << 25U) |
                        (static_cast<std::uint32_t>(relocation.external) << 27U) |
                        (static_cast<std::uint32_t>(relocation.type) << 28U));
            }
        }
        else {
            for (const auto &relocation : file.sections[i].relocs) {
                out.U32(relocation.sectionOffset);
                const bool pcRelative = relocation.type == RcuRelType::Rel32;
                const std::uint32_t length = relocation.type == RcuRelType::Abs64 ? 3 : 2;
                const auto &target = file.symbols[relocation.symbolIndex];
                const std::uint32_t type = pcRelative && target.kind == RcuSymKind::Func ? 2U : pcRelative ? 1U : 0U;
                out.U32(symbolRemap[relocation.symbolIndex] | (static_cast<std::uint32_t>(pcRelative) << 24U) |
                        (length << 25U) | (1U << 27U) | (type << 28U));
            }
        }
    }
    for (const std::size_t oldIndex : order) {
        const auto &symbol = file.symbols[oldIndex];
        const std::string nativeName = "_" + symbol.name;
        out.U32(AddString(strings, nativeName));
        const bool external = symbol.visibility != RcuSymVis::Local;
        out.U8(static_cast<std::uint8_t>((symbol.sectionIdx == RCU_SEC_EXTERNAL ? 0 : 0x0e) | (external ? 1 : 0)));
        out.U8(symbol.sectionIdx == RCU_SEC_EXTERNAL ? 0 : static_cast<std::uint8_t>(symbol.sectionIdx + 1));
        out.U16(0);
        out.U64(symbol.value);
        if (external && symbol.sectionIdx != RCU_SEC_EXTERNAL) {
            output.publicSymbols.push_back(nativeName);
        }
    }
    out.Raw(strings);
    out.Patch32(stringSizePatch, static_cast<std::uint32_t>(strings.size()));
    output.bytes = std::move(out.data);
    return true;
}
} // namespace

bool WriteNativeObject(const RcuFile &file, const Target::OS targetOs, const Target::Arch targetArch,
                       NativeObject &output, std::string &error) {
    output = {};
    const std::uint8_t expectedArch = RcuArchFor(targetArch);
    if (expectedArch == RcuArch::Unknown) {
        error = std::format("cannot write an object for {}: no object encoding exists for this architecture",
                            Target::ToDisplayString(targetArch));
        return false;
    }
    if (file.arch != expectedArch) {
        error = std::format("object was compiled for {}, but the target is {}", RcuArchName(file.arch),
                            RcuArchName(expectedArch));
        return false;
    }
    if (targetOs == Target::OS::Windows) {
        return WriteCoffObject(file, targetArch, output, error);
    }
    const std::string_view container = targetOs == Target::OS::MacOS ? "Mach-O" : "ELF";
    for (const auto &section : file.sections) {
        for (const auto &relocation : section.relocs) {
            const bool writable = targetOs == Target::OS::MacOS
                                    ? targetArch == Target::Arch::AArch64 ? IsAArch64MachORelocation(relocation.type)
                                                                          : IsFieldRelocation(relocation.type)
                                    : ElfRelocationType(relocation.type, targetArch,
                                                        InstructionAt(section.data, relocation.sectionOffset))
                                          .has_value();
            if (!writable) {
                error = std::format("relocation {} in section {} is not supported by the {} object writer",
                                    RcuRelTypeName(relocation.type), section.name, container);
                return false;
            }
            if (targetArch == Target::Arch::AArch64 && targetOs != Target::OS::MacOS &&
                !ValidateAArch64ElfRelocation(section, relocation, file.symbols[relocation.symbolIndex].name, error)) {
                return false;
            }
        }
    }
    output.name = std::filesystem::path(file.sourcePath).stem().string();
    if (output.name.empty()) {
        output.name = "out";
    }
    output.name += ".o";
    if (targetOs == Target::OS::MacOS) {
        return WriteMachO(file, targetArch, output, error);
    }
    const auto profile = Target::Elf64ProfileFor(targetOs, targetArch);
    if (!profile) {
        error = std::format("ELF object writer does not implement the complete target '{}-{}'",
                            Target::ToString(targetOs), Target::ToString(targetArch));
        return false;
    }
    return WriteElf(file, *profile, output, error);
}
} // namespace Rux
