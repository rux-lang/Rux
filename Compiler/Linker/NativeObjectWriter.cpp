#include "Linker/NativeObjectWriter.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <numeric>
#include <span>
#include <string_view>

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

std::vector<std::size_t> SymbolOrder(const RcuFile &file) {
    std::vector<std::size_t> order(file.symbols.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_partition(order.begin(), order.end(),
                          [&](const std::size_t index) { return file.symbols[index].visibility == RcuSymVis::Local; });
    return order;
}

void ApplyInlineAddend(std::vector<std::uint8_t> &data, const RcuReloc &relocation, const bool elf) {
    const std::int64_t addend = relocation.type == RcuRelType::Rel32 && elf ? relocation.addend - 4 : relocation.addend;
    const std::size_t width = relocation.type == RcuRelType::Abs64 ? 8 : 4;
    if (static_cast<std::size_t>(relocation.sectionOffset) + width > data.size()) {
        return;
    }
    for (std::size_t i = 0; i < width; ++i) {
        data[relocation.sectionOffset + i] = static_cast<std::uint8_t>(static_cast<std::uint64_t>(addend) >> (i * 8U));
    }
}

bool WriteCoff(const RcuFile &file, NativeObject &output, std::string &error) {
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
            ApplyInlineAddend(sectionData.back(), relocation, false);
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
    out.U16(0x8664);
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
            const std::uint16_t type = relocation.type == RcuRelType::Abs64 ? 0x0001
                                     : relocation.type == RcuRelType::Abs32 ? 0x0002
                                                                            : 0x0004;
            out.U16(type);
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

bool WriteElf(const RcuFile &file, NativeObject &output, std::string &error) {
    (void)error;
    std::vector<ElfSection> sections(1);
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
        relocationSection.alignment = 8;
        relocationSection.entrySize = 24;
        relocationSection.info = static_cast<std::uint32_t>(i + 1);
        Bytes relocations;
        for (const auto &relocation : file.sections[i].relocs) {
            const std::uint32_t type = relocation.type == RcuRelType::Abs64 ? 1
                                     : relocation.type == RcuRelType::Abs32 ? 10
                                                                            : 2;
            relocations.U64(relocation.sectionOffset);
            relocations.U64((static_cast<std::uint64_t>(symbolRemap[relocation.symbolIndex]) << 32U) | type);
            relocations.I64(relocation.type == RcuRelType::Rel32 ? relocation.addend - 4 : relocation.addend);
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
    out.Patch32(16, 1U | (62U << 16U));
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

bool WriteMachO(const RcuFile &file, NativeObject &output, std::string &error) {
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
            ApplyInlineAddend(sectionData.back(), relocation, false);
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

    Bytes out;
    out.U32(0xfeedfacf);
    out.U32(0x01000007);
    out.U32(3);
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

bool WriteNativeObject(const RcuFile &file, const Target::OS targetOs, NativeObject &output, std::string &error) {
    output = {};
    output.name = std::filesystem::path(file.sourcePath).stem().string();
    if (output.name.empty()) {
        output.name = "out";
    }
    output.name += targetOs == Target::OS::Windows ? ".obj" : ".o";
    if (targetOs == Target::OS::Windows) {
        return WriteCoff(file, output, error);
    }
    if (targetOs == Target::OS::MacOS) {
        return WriteMachO(file, output, error);
    }
    return WriteElf(file, output, error);
}
} // namespace Rux
