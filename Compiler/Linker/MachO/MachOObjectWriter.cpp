#include "Linker/MachO/MachOObjectWriter.h"

#include "Linker/AArch64Relocation.h"
#include "Linker/LinkerInternal.h"
#include "Linker/NativeObjectWriter.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <numeric>
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

// The instruction a relocation names, or zero when the site lies past the end
// of its section.
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

bool IsAArch64Adrp(const std::uint32_t word) noexcept {
    return (word & 0x9F00'0000U) == 0x9000'0000U;
}

bool IsAArch64UnshiftedAddImmediate(const std::uint32_t word) noexcept {
    return (word & 0x7F80'0000U) == 0x1100'0000U && (word & 0x0040'0000U) == 0;
}

bool IsAArch64UnsignedOffsetLoadStore(const std::uint32_t word) noexcept {
    return (word & 0x3B00'0000U) == 0x3900'0000U;
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

bool SerializeMachOObject(const RcuFile &file, const Target::Arch targetArch, NativeObject &output,
                          std::string &error) {
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

bool WriteMachOObject(const RcuFile &file, const Target::Arch targetArch, NativeObject &output, std::string &error) {
    for (const auto &section : file.sections) {
        for (const auto &relocation : section.relocs) {
            const bool writable = targetArch == Target::Arch::AArch64 ? IsAArch64MachORelocation(relocation.type)
                                                                      : IsFieldRelocation(relocation.type);
            if (!writable) {
                error = std::format("relocation {} in section {} is not supported by the Mach-O object writer",
                                    RcuRelTypeName(relocation.type), section.name);
                return false;
            }
        }
    }
    output.name = std::filesystem::path(file.sourcePath).stem().string();
    if (output.name.empty()) {
        output.name = "out";
    }
    output.name += ".o";
    return SerializeMachOObject(file, targetArch, output, error);
}
} // namespace Rux
