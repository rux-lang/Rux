#include "Linker/Coff/CoffObjectWriter.h"

#include "Linker/AArch64Relocation.h"
#include "Linker/LinkerInternal.h"
#include "Linker/NativeObjectWriter.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <limits>
#include <numeric>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace Rux {
namespace {
std::uint32_t AddString(std::vector<std::uint8_t> &table, const std::string_view value) {
    const auto offset = static_cast<std::uint32_t>(table.size());
    table.insert(table.end(), value.begin(), value.end());
    table.push_back(0);
    return offset;
}

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

bool WriteCoffObject(const RcuFile &file, const Target::Arch targetArch, NativeObject &output, std::string &error) {
    for (const auto &section : file.sections) {
        for (const auto &relocation : section.relocs) {
            if (!CoffRelocationType(relocation.type, targetArch)) {
                error = std::format("relocation {} in section {} is not supported by the COFF object writer",
                                    RcuRelTypeName(relocation.type), section.name);
                return false;
            }
        }
    }
    output.name = std::filesystem::path(file.sourcePath).stem().string();
    if (output.name.empty()) {
        output.name = "out";
    }
    output.name += ".obj";
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
    Buf out;
    WriteU16(out, CoffMachine(targetArch));
    WriteU16(out, static_cast<std::uint16_t>(sectionCount));
    WriteU32(out, 0);
    WriteU32(out, symbolTableOffset);
    WriteU32(out, static_cast<std::uint32_t>(file.symbols.size()));
    WriteU16(out, 0);
    WriteU16(out, 0);

    for (std::size_t i = 0; i < sectionCount; ++i) {
        const auto &section = file.sections[i];
        WriteName8(out, section.name.c_str());
        WriteU32(out, 0);
        WriteU32(out, 0);
        WriteU32(out, static_cast<std::uint32_t>(sectionData[i].size()));
        WriteU32(out, rawOffsets[i]);
        WriteU32(out, relocationOffsets[i]);
        WriteU32(out, 0);
        WriteU16(out, static_cast<std::uint16_t>(section.relocs.size()));
        WriteU16(out, 0);
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
        WriteU32(out, characteristics);
    }
    for (std::size_t i = 0; i < sectionCount; ++i) {
        out.insert(out.end(), sectionData[i].begin(), sectionData[i].end());
        for (const auto &relocation : file.sections[i].relocs) {
            WriteU32(out, relocation.sectionOffset);
            WriteU32(out, symbolRemap[relocation.symbolIndex]);
            WriteU16(out, *CoffRelocationType(relocation.type, targetArch));
        }
    }
    for (const std::size_t oldIndex : order) {
        const auto &symbol = file.symbols[oldIndex];
        if (symbol.name.size() <= 8) {
            WriteName8(out, symbol.name.c_str());
        }
        else {
            WriteU32(out, 0);
            WriteU32(out, AddString(stringTable, symbol.name));
        }
        WriteU32(out, symbol.value);
        const std::int16_t section = symbol.sectionIdx == RCU_SEC_EXTERNAL ? 0
                                   : symbol.sectionIdx == RCU_SEC_ABSOLUTE
                                       ? -1
                                       : static_cast<std::int16_t>(symbol.sectionIdx + 1);
        WriteU16(out, static_cast<std::uint16_t>(section));
        WriteU16(out, symbol.kind == RcuSymKind::Func ? 0x20 : 0);
        WriteU8(out, symbol.visibility == RcuSymVis::Local ? 3 : 2);
        WriteU8(out, 0);
        if (symbol.visibility != RcuSymVis::Local && symbol.sectionIdx != RCU_SEC_EXTERNAL) {
            output.publicSymbols.push_back(symbol.name);
        }
    }
    const auto stringSize = static_cast<std::uint32_t>(stringTable.size());
    for (std::size_t i = 0; i < 4; ++i) {
        stringTable[i] = static_cast<std::uint8_t>(stringSize >> (i * 8U));
    }
    out.insert(out.end(), stringTable.begin(), stringTable.end());
    output.bytes = std::move(out);
    return true;
}
} // namespace Rux
