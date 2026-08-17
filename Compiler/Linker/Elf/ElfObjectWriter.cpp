// Translates one RCU object into a relocatable ELF object, so a Rux static
// library can be consumed by a toolchain that cannot read RCU. Linking a whole
// ELF image is ElfWriter's job.

#include "Linker/Elf/ElfObjectWriter.h"

#include "Linker/AArch64Relocation.h"
#include "Linker/NativeObjectWriter.h"
#include "Target/ElfProfile.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <numeric>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

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

/// Intern a name into an ELF string table and return its offset, the form every ELF name field takes.
std::uint32_t AddString(std::vector<std::uint8_t> &table, const std::string_view value) {
    const auto offset = static_cast<std::uint32_t>(table.size());
    table.insert(table.end(), value.begin(), value.end());
    table.push_back(0);
    return offset;
}

/// Read the 32-bit instruction word at an offset. AArch64 relocation types depend on which instruction is being
/// patched, so the writer has to decode the site before it can classify the fixup.
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

/// Order symbols so every local one precedes every global one. ELF requires that split and records the boundary in the
/// section header, so the order is part of the format rather than a preference.
std::vector<std::size_t> SymbolOrder(const RcuFile &file) {
    std::vector<std::size_t> order(file.symbols.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_partition(order.begin(), order.end(),
                          [&](const std::size_t index) { return file.symbols[index].visibility == RcuSymVis::Local; });
    return order;
}

/// R_AARCH64_LDST<n>_ABS_LO12_NC, one number per access width, because a linker placing the symbol's low 12 bits into
/// the immediate has to scale them down by the width the instruction moves.
std::uint32_t AArch64LdstRelocationType(const std::uint32_t word) noexcept {
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

/// The ELF relocation number a kind takes for `targetArch`, or nullopt when that architecture has none for it. `word`
/// is read only by the load-store form, whose ELF relocation number depends on the instruction access width.
std::optional<std::uint32_t> RelocationType(const std::uint16_t type, const Target::Arch targetArch,
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
        return AArch64LdstRelocationType(word);
    default:
        return std::nullopt;
    }
}

/// The addend for a relocation, which x86-64 and AArch64 carry differently in the RCU form.
///
/// R_X86_64_PC32 is measured from the end of its four-byte field. AArch64 relocations are measured from the start of
/// the instruction or field.
std::int64_t RelocationAddend(const RcuReloc &relocation, const Target::Arch targetArch) noexcept {
    const bool biased = targetArch == Target::Arch::X86_64 && relocation.type == RcuRelType::Rel32;
    return biased ? relocation.addend - 4 : relocation.addend;
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

bool ValidateAArch64Relocation(const RcuSection &section, const RcuReloc &relocation, const std::string_view symbolName,
                               std::string &error) {
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

bool Serialize(const RcuFile &file, const Target::Elf64Profile &profile, NativeObject &output) {
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
            const std::uint32_t type = RelocationType(relocation.type, targetArch,
                                                      InstructionAt(file.sections[i].data, relocation.sectionOffset))
                                           .value_or(0);
            relocations.U64(relocation.sectionOffset);
            relocations.U64((static_cast<std::uint64_t>(symbolRemap[relocation.symbolIndex]) << 32U) | type);
            relocations.I64(RelocationAddend(relocation, targetArch));
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
} // namespace

bool WriteElfObject(const RcuFile &file, const Target::OS targetOs, const Target::Arch targetArch, NativeObject &output,
                    std::string &error) {
    for (const auto &section : file.sections) {
        for (const auto &relocation : section.relocs) {
            if (!RelocationType(relocation.type, targetArch, InstructionAt(section.data, relocation.sectionOffset))) {
                error = std::format("relocation {} in section {} is not supported by the ELF object writer",
                                    RcuRelTypeName(relocation.type), section.name);
                return false;
            }
            if (targetArch == Target::Arch::AArch64 &&
                !ValidateAArch64Relocation(section, relocation, file.symbols[relocation.symbolIndex].name, error)) {
                return false;
            }
        }
    }
    output.name = std::filesystem::path(file.sourcePath).stem().string();
    if (output.name.empty()) {
        output.name = "out";
    }
    output.name += ".o";
    const auto profile = Target::Elf64ProfileFor(targetOs, targetArch);
    if (!profile) {
        error = std::format("ELF object writer does not implement the complete target '{}-{}'",
                            Target::ToString(targetOs), Target::ToString(targetArch));
        return false;
    }
    return Serialize(file, *profile, output);
}
} // namespace Rux
