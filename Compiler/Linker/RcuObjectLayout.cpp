#include "Linker/RcuObjectLayout.h"

#include <algorithm>
#include <limits>

namespace Rux {
namespace {
[[nodiscard]] std::optional<RcuMergedSection> MergedSectionFor(const std::uint32_t type) {
    switch (type) {
    case RcuSecType::Text:
        return RcuMergedSection::Text;
    case RcuSecType::RoData:
        return RcuMergedSection::RoData;
    case RcuSecType::Data:
        return RcuMergedSection::Data;
    case RcuSecType::Bss:
        return RcuMergedSection::Bss;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<RcuMergedSection> MergedSectionForSymbolIndex(const std::uint16_t sectionIndex) {
    switch (sectionIndex) {
    case RCU_TEXT_IDX:
        return RcuMergedSection::Text;
    case RCU_RODATA_IDX:
        return RcuMergedSection::RoData;
    case RCU_DATA_IDX:
        return RcuMergedSection::Data;
    case RCU_BSS_IDX:
        return RcuMergedSection::Bss;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] std::size_t Index(const RcuMergedSection section) {
    return static_cast<std::size_t>(section);
}

/// Pad to an alignment boundary with a chosen filler, which differs by section: code is padded with a trap byte so
/// execution running off the end of a function faults instead of drifting into the next one.
void Pad(std::vector<std::uint8_t> &data, const std::uint16_t alignment, const std::uint8_t value) {
    const auto boundary = std::max<std::uint16_t>(alignment, 1);
    while (data.size() % boundary != 0) {
        data.push_back(value);
    }
}

[[nodiscard]] std::uint64_t Align(const std::uint64_t value, const std::uint16_t alignment) {
    const auto boundary = std::max<std::uint16_t>(alignment, 1);
    const auto remainder = value % boundary;
    return remainder == 0 ? value : value + boundary - remainder;
}
} // namespace

RcuObjectLayout RcuObjectLayout::Build(const std::span<const RcuFile> inputObjects, const RcuLayoutPrefixes &prefixes) {
    RcuObjectLayout layout;
    layout.data[Index(RcuMergedSection::Text)].assign(prefixes.text.begin(), prefixes.text.end());
    layout.data[Index(RcuMergedSection::RoData)].assign(prefixes.rodata.begin(), prefixes.rodata.end());
    layout.data[Index(RcuMergedSection::Data)].assign(prefixes.data.begin(), prefixes.data.end());
    layout.objects.resize(inputObjects.size());

    const std::array padding = {prefixes.textPadding, prefixes.rodataPadding, prefixes.dataPadding};
    for (std::size_t objectIndex = 0; objectIndex < inputObjects.size(); ++objectIndex) {
        const RcuFile &object = inputObjects[objectIndex];
        auto &objectPlacement = layout.objects[objectIndex];
        objectPlacement.sections.resize(object.sections.size());
        objectPlacement.relocations.resize(object.sections.size());

        std::array<std::uint16_t, 4> objectAlignments = {1, 1, 1, 1};
        for (const RcuSection &section : object.sections) {
            if (const auto merged = MergedSectionFor(section.type)) {
                const auto index = Index(*merged);
                objectAlignments[index] = std::max(objectAlignments[index], section.alignment);
                layout.alignments[index] = std::max(layout.alignments[index], section.alignment);
            }
        }

        std::array<std::uint64_t, 4> offsets = {};
        for (std::size_t sectionIndex = 0; sectionIndex < 3; ++sectionIndex) {
            Pad(layout.data[sectionIndex], objectAlignments[sectionIndex], padding[sectionIndex]);
            offsets[sectionIndex] = layout.data[sectionIndex].size();
        }
        layout.bssSize = Align(layout.bssSize, objectAlignments[Index(RcuMergedSection::Bss)]);
        offsets[Index(RcuMergedSection::Bss)] = layout.bssSize;

        for (std::size_t sectionIndex = 0; sectionIndex < object.sections.size(); ++sectionIndex) {
            const RcuSection &section = object.sections[sectionIndex];
            const auto merged = MergedSectionFor(section.type);
            if (!merged) {
                continue;
            }
            objectPlacement.sections[sectionIndex] = RcuSectionPlacement{*merged, offsets[Index(*merged)]};
            if (*merged == RcuMergedSection::Bss) {
                layout.bssSize += section.data.size();
                offsets[Index(*merged)] = layout.bssSize;
                continue;
            }
            auto &output = layout.data[Index(*merged)];
            output.insert(output.end(), section.data.begin(), section.data.end());
            offsets[Index(*merged)] = output.size();
        }

        objectPlacement.symbols.resize(object.symbols.size());
        for (std::size_t symbolIndex = 0; symbolIndex < object.symbols.size(); ++symbolIndex) {
            const RcuSymbol &symbol = object.symbols[symbolIndex];
            const auto merged = MergedSectionForSymbolIndex(symbol.sectionIdx);
            if (!merged) {
                continue;
            }
            const auto section = std::ranges::find_if(object.sections, [&](const RcuSection &candidate) {
                return MergedSectionFor(candidate.type) == merged;
            });
            if (section == object.sections.end()) {
                continue;
            }
            if (symbol.value > section->data.size() || symbol.size > section->data.size() - symbol.value) {
                continue;
            }
            const auto physicalSectionIndex = static_cast<std::size_t>(section - object.sections.begin());
            const RcuSectionPlacement sectionPlacement = *objectPlacement.sections[physicalSectionIndex];
            objectPlacement.symbols[symbolIndex] =
                RcuSectionPlacement{sectionPlacement.section, sectionPlacement.offset + symbol.value};
        }
        for (std::size_t sectionIndex = 0; sectionIndex < object.sections.size(); ++sectionIndex) {
            const RcuSection &section = object.sections[sectionIndex];
            auto &relocations = objectPlacement.relocations[sectionIndex];
            relocations.resize(section.relocs.size());
            if (!objectPlacement.sections[sectionIndex]) {
                continue;
            }
            const RcuSectionPlacement sectionPlacement = *objectPlacement.sections[sectionIndex];
            for (std::size_t relocationIndex = 0; relocationIndex < section.relocs.size(); ++relocationIndex) {
                const RcuReloc &relocation = section.relocs[relocationIndex];
                if (relocation.sectionOffset < section.data.size()) {
                    relocations[relocationIndex] = RcuSectionPlacement{
                        sectionPlacement.section, sectionPlacement.offset + relocation.sectionOffset};
                }
            }
        }
    }
    return layout;
}

const std::vector<std::uint8_t> &RcuObjectLayout::Data(const RcuMergedSection section) const {
    static const std::vector<std::uint8_t> empty;
    return section == RcuMergedSection::Bss ? empty : data.at(Index(section));
}

std::uint16_t RcuObjectLayout::Alignment(const RcuMergedSection section) const {
    return alignments.at(Index(section));
}

std::optional<RcuSectionPlacement> RcuObjectLayout::Section(const std::size_t objectIndex,
                                                            const std::size_t sectionIndex) const {
    if (objectIndex >= objects.size() || sectionIndex >= objects[objectIndex].sections.size()) {
        return std::nullopt;
    }
    return objects[objectIndex].sections[sectionIndex];
}

std::optional<RcuSectionPlacement> RcuObjectLayout::Symbol(const RcuSymbolLocation location) const {
    if (location.objectIndex >= objects.size() ||
        location.symbolIndex >= objects[location.objectIndex].symbols.size()) {
        return std::nullopt;
    }
    return objects[location.objectIndex].symbols[location.symbolIndex];
}

std::optional<RcuSectionPlacement> RcuObjectLayout::Relocation(const std::size_t objectIndex,
                                                               const std::size_t sectionIndex,
                                                               const std::size_t relocationIndex) const {
    if (objectIndex >= objects.size() || sectionIndex >= objects[objectIndex].relocations.size() ||
        relocationIndex >= objects[objectIndex].relocations[sectionIndex].size()) {
        return std::nullopt;
    }
    return objects[objectIndex].relocations[sectionIndex][relocationIndex];
}

std::optional<std::uint64_t> RcuObjectLayout::Address(const RcuSectionPlacement placement,
                                                      const RcuSectionBases &bases) {
    std::uint64_t base = 0;
    switch (placement.section) {
    case RcuMergedSection::Text:
        base = bases.text;
        break;
    case RcuMergedSection::RoData:
        base = bases.rodata;
        break;
    case RcuMergedSection::Data:
        base = bases.data;
        break;
    case RcuMergedSection::Bss:
        base = bases.bss;
        break;
    }
    if (placement.offset > std::numeric_limits<std::uint64_t>::max() - base) {
        return std::nullopt;
    }
    return base + placement.offset;
}
} // namespace Rux
