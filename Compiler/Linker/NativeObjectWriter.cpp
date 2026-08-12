#include "Linker/NativeObjectWriter.h"

#include "Linker/AArch64Relocation.h"
#include "Linker/LinkerInternal.h"

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

// ELF e_machine: EM_X86_64 and EM_AARCH64.
std::uint16_t ElfMachine(const Target::Arch targetArch) noexcept {
    switch (targetArch) {
    case Target::Arch::X86_64:
        return 62;
    case Target::Arch::AArch64:
        return 183;
    default:
        return 0;
    }
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

// The COFF relocation number a kind takes for `targetArch`, or nullopt when
// that architecture has no representation for it. Classic Windows ARM64 has
// no relocation for the MOVW halfwords or a 64-bit PC-relative data field.
std::optional<std::uint16_t> CoffRelocationType(const std::uint16_t type, const Target::Arch targetArch) noexcept {
    if (targetArch == Target::Arch::X86_64) {
        switch (type) {
        case RcuRelType::None:
            return 0x0000; // IMAGE_REL_AMD64_ABSOLUTE
        case RcuRelType::Abs64:
            return 0x0001; // IMAGE_REL_AMD64_ADDR64
        case RcuRelType::Abs32:
            return 0x0002; // IMAGE_REL_AMD64_ADDR32
        case RcuRelType::Rel32:
            return 0x0004; // IMAGE_REL_AMD64_REL32
        default:
            return std::nullopt;
        }
    }
    switch (type) {
    case RcuRelType::None:
        return 0x0000; // IMAGE_REL_ARM64_ABSOLUTE
    case RcuRelType::Abs32:
        return 0x0001; // IMAGE_REL_ARM64_ADDR32
    case RcuRelType::AArch64Call26:
    case RcuRelType::AArch64Jump26:
        return 0x0003; // IMAGE_REL_ARM64_BRANCH26
    case RcuRelType::AArch64AdrPrelPgHi21:
        return 0x0004; // IMAGE_REL_ARM64_PAGEBASE_REL21
    case RcuRelType::AArch64AddAbsLo12Nc:
        return 0x0006; // IMAGE_REL_ARM64_PAGEOFFSET_12A
    case RcuRelType::AArch64LdstAbsLo12Nc:
        return 0x0007; // IMAGE_REL_ARM64_PAGEOFFSET_12L
    case RcuRelType::Abs64:
        return 0x000E; // IMAGE_REL_ARM64_ADDR64
    case RcuRelType::AArch64CondBr19:
        return 0x000F; // IMAGE_REL_ARM64_BRANCH19
    case RcuRelType::AArch64TstBr14:
        return 0x0010; // IMAGE_REL_ARM64_BRANCH14
    case RcuRelType::Rel32:
    case RcuRelType::AArch64Prel32:
        return 0x0011; // IMAGE_REL_ARM64_REL32
    default:
        return std::nullopt;
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

// COFF has no explicit addend field. Whole-field relocations carry it as a
// little-endian value, while ARM64 instruction relocations carry it in their
// immediate bits using the COFF linker's input convention.
bool ApplyCoffInlineAddend(Buf &data, const RcuReloc &relocation, const Target::Arch targetArch,
                           const std::string_view symbolName, std::string &error) {
    if (relocation.type == RcuRelType::None) {
        return true;
    }
    if (targetArch == Target::Arch::X86_64 || IsFieldRelocation(relocation.type)) {
        PatchInlineField(data, relocation, relocation.addend);
        return true;
    }
    if (relocation.type == RcuRelType::AArch64Prel32) {
        // IMAGE_REL_ARM64_REL32 measures from the byte after its four-byte
        // field; PREL32 measures from the field itself.
        const std::int64_t biased = static_cast<std::int64_t>(relocation.addend) + 4;
        if (biased > std::numeric_limits<std::int32_t>::max()) {
            error = std::format("AARCH64_PREL32 relocation against '{}' has an inline addend that does not fit in "
                                "32 bits after the COFF PC bias",
                                symbolName);
            return false;
        }
        PatchInlineField(data, relocation, biased);
        return true;
    }
    if (relocation.type == RcuRelType::AArch64AdrPrelPgHi21) {
        // COFF stores the byte addend in the split ADRP immediate. The linker
        // adds it to the symbol before taking the page difference; encoding
        // the addend as a page count here would lose its low twelve bits.
        constexpr std::int64_t limit = std::int64_t{1} << 20;
        if (relocation.addend < -limit || relocation.addend >= limit) {
            error = std::format("AARCH64_ADR_PREL_PG_HI21 relocation against '{}' has an inline addend that does "
                                "not fit in the signed 21-bit COFF field",
                                symbolName);
            return false;
        }
        if (static_cast<std::size_t>(relocation.sectionOffset) + 4 > data.size()) {
            return true;
        }
        const std::uint32_t word = InstructionAt(data, relocation.sectionOffset);
        const std::uint32_t immediate = static_cast<std::uint32_t>(relocation.addend) & 0x1FFFFFU;
        constexpr std::uint32_t mask = (0x3U << 29U) | (0x7FFFFU << 5U);
        const std::uint32_t patched = (word & ~mask) | ((immediate & 3U) << 29U) | ((immediate >> 2U) << 5U);
        Patch32(data, relocation.sectionOffset, patched);
        return true;
    }
    return ApplyAArch64Relocation(data, relocation.sectionOffset, relocation.type, 0, relocation.addend, 0, symbolName,
                                  "COFF object writer", error);
}

bool WriteCoff(const RcuFile &file, const Target::Arch targetArch, NativeObject &output, std::string &error) {
    if (file.sections.size() > std::numeric_limits<std::uint16_t>::max()) {
        error = "too many sections for a COFF object";
        return false;
    }
    const std::size_t sectionCount = file.sections.size();
    const std::size_t headersSize = 20 + sectionCount * 40;
    std::vector<std::vector<std::uint8_t>> sectionData;
    sectionData.reserve(sectionCount);
    std::vector<std::uint32_t> rawOffsets(sectionCount);
    std::vector<std::uint32_t> relocationOffsets(sectionCount);
    std::size_t cursor = headersSize;
    for (std::size_t i = 0; i < sectionCount; ++i) {
        sectionData.push_back(file.sections[i].data);
        for (const auto &relocation : file.sections[i].relocs) {
            if (!ApplyCoffInlineAddend(sectionData.back(), relocation, targetArch,
                                       file.symbols[relocation.symbolIndex].name, error)) {
                return false;
            }
        }
        rawOffsets[i] = sectionData.back().empty() ? 0 : static_cast<std::uint32_t>(cursor);
        cursor += sectionData.back().size();
        relocationOffsets[i] = file.sections[i].relocs.empty() ? 0 : static_cast<std::uint32_t>(cursor);
        cursor += file.sections[i].relocs.size() * 10;
    }

    const auto order = SymbolOrder(file);
    std::vector<std::uint32_t> symbolRemap(file.symbols.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        symbolRemap[order[i]] = static_cast<std::uint32_t>(i);
    }
    const auto symbolTableOffset = static_cast<std::uint32_t>(cursor);

    std::vector<std::uint8_t> stringTable(4);
    Bytes out;
    out.U16(CoffMachine(targetArch));
    out.U16(static_cast<std::uint16_t>(sectionCount));
    out.U32(0);
    out.U32(symbolTableOffset);
    out.U32(static_cast<std::uint32_t>(file.symbols.size()));
    out.U16(0);
    out.U16(0);

    for (std::size_t i = 0; i < sectionCount; ++i) {
        const auto &section = file.sections[i];
        out.Fixed(section.name, 8);
        out.U32(0);
        out.U32(0);
        out.U32(static_cast<std::uint32_t>(sectionData[i].size()));
        out.U32(rawOffsets[i]);
        out.U32(relocationOffsets[i]);
        out.U32(0);
        out.U16(static_cast<std::uint16_t>(section.relocs.size()));
        out.U16(0);
        std::uint32_t characteristics = 0x40000000U;
        if ((section.flags & RcuSecFlag::Exec) != 0) {
            characteristics |= 0x20000000U | 0x00000020U;
        }
        else if ((section.flags & RcuSecFlag::Write) != 0) {
            characteristics |= 0x80000000U | 0x00000040U;
        }
        else {
            characteristics |= 0x00000040U;
        }
        out.U32(characteristics);
    }
    for (std::size_t i = 0; i < sectionCount; ++i) {
        out.Raw(sectionData[i]);
        for (const auto &relocation : file.sections[i].relocs) {
            out.U32(relocation.sectionOffset);
            out.U32(symbolRemap[relocation.symbolIndex]);
            out.U16(*CoffRelocationType(relocation.type, targetArch));
        }
    }
    for (const std::size_t oldIndex : order) {
        const auto &symbol = file.symbols[oldIndex];
        if (symbol.name.size() <= 8) {
            out.Fixed(symbol.name, 8);
        }
        else {
            out.U32(0);
            out.U32(AddString(stringTable, symbol.name));
        }
        out.U32(symbol.value);
        const std::int16_t section = symbol.sectionIdx == RCU_SEC_EXTERNAL ? 0
                                   : symbol.sectionIdx == RCU_SEC_ABSOLUTE
                                       ? -1
                                       : static_cast<std::int16_t>(symbol.sectionIdx + 1);
        out.U16(static_cast<std::uint16_t>(section));
        out.U16(symbol.kind == RcuSymKind::Func ? 0x20 : 0);
        out.U8(symbol.visibility == RcuSymVis::Local ? 3 : 2);
        out.U8(0);
        if (symbol.visibility != RcuSymVis::Local && symbol.sectionIdx != RCU_SEC_EXTERNAL) {
            output.publicSymbols.push_back(symbol.name);
        }
    }
    const auto stringSize = static_cast<std::uint32_t>(stringTable.size());
    for (std::size_t i = 0; i < 4; ++i) {
        stringTable[i] = static_cast<std::uint8_t>(stringSize >> (i * 8U));
    }
    out.Raw(stringTable);
    output.bytes = std::move(out.data);
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

bool WriteElf(const RcuFile &file, const Target::Arch targetArch, NativeObject &output, std::string &error) {
    (void)error;
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
    const std::array<std::uint8_t, 16> ident = {0x7f, 'E', 'L', 'F', 2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    std::copy(ident.begin(), ident.end(), out.data.begin());
    out.Patch32(16, 1U | (static_cast<std::uint32_t>(ElfMachine(targetArch)) << 16U));
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
    std::vector<std::vector<std::uint8_t>> sectionData;
    std::vector<std::uint32_t> sectionOffsets(sectionCount);
    std::vector<std::uint32_t> relocationOffsets(sectionCount);
    std::size_t cursor = dataStart;
    for (std::size_t i = 0; i < sectionCount; ++i) {
        sectionData.push_back(file.sections[i].data);
        for (const auto &relocation : file.sections[i].relocs) {
            PatchInlineField(sectionData.back(), relocation, relocation.addend);
        }
        sectionOffsets[i] = static_cast<std::uint32_t>(cursor);
        cursor += sectionData.back().size();
    }
    for (std::size_t i = 0; i < sectionCount; ++i) {
        relocationOffsets[i] = file.sections[i].relocs.empty() ? 0 : static_cast<std::uint32_t>(cursor);
        cursor += file.sections[i].relocs.size() * 8;
    }
    const auto order = SymbolOrder(file);
    std::vector<std::uint32_t> symbolRemap(file.symbols.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        symbolRemap[order[i]] = static_cast<std::uint32_t>(i);
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
        out.U32(static_cast<std::uint32_t>(section.relocs.size()));
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

std::uint16_t CoffMachine(const Target::Arch targetArch) noexcept {
    switch (targetArch) {
    case Target::Arch::X86_64:
        return 0x8664;
    case Target::Arch::AArch64:
        return 0xAA64;
    default:
        return 0;
    }
}

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
    const std::string_view container = targetOs == Target::OS::Windows ? "COFF"
                                     : targetOs == Target::OS::MacOS   ? "Mach-O"
                                                                       : "ELF";
    for (const auto &section : file.sections) {
        for (const auto &relocation : section.relocs) {
            const bool writable = targetOs == Target::OS::Windows
                                    ? CoffRelocationType(relocation.type, targetArch).has_value()
                                : targetOs == Target::OS::MacOS
                                    ? IsFieldRelocation(relocation.type)
                                    : ElfRelocationType(relocation.type, targetArch,
                                                        InstructionAt(section.data, relocation.sectionOffset))
                                          .has_value();
            if (!writable) {
                error = std::format("relocation {} in section {} is not supported by the {} object writer",
                                    RcuRelTypeName(relocation.type), section.name, container);
                return false;
            }
        }
    }
    output.name = std::filesystem::path(file.sourcePath).stem().string();
    if (output.name.empty()) {
        output.name = "out";
    }
    output.name += targetOs == Target::OS::Windows ? ".obj" : ".o";
    if (targetOs == Target::OS::Windows) {
        return WriteCoff(file, targetArch, output, error);
    }
    if (targetOs == Target::OS::MacOS) {
        return WriteMachO(file, targetArch, output, error);
    }
    return WriteElf(file, targetArch, output, error);
}
} // namespace Rux
