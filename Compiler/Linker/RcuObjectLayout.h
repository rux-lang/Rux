#pragma once

// Target-format-neutral placement of RCU input sections. Format writers own
// their generated prefixes and final image sections; this class owns the
// alignment-sensitive concatenation between those boundaries.

#include "Linker/RcuLinkGraph.h"
#include "Object/Rcu/Rcu.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Rux {
enum class RcuMergedSection : std::uint8_t {
    Text,
    RoData,
    Data,
    Bss,
};

struct RcuLayoutPrefixes {
    std::span<const std::uint8_t> text;
    std::span<const std::uint8_t> rodata;
    std::span<const std::uint8_t> data;
    std::uint8_t textPadding = 0;
    std::uint8_t rodataPadding = 0;
    std::uint8_t dataPadding = 0;
};

struct RcuSectionPlacement {
    RcuMergedSection section = RcuMergedSection::Text;
    std::uint64_t offset = 0;

    auto operator<=>(const RcuSectionPlacement &) const = default;
};

struct RcuSectionBases {
    std::uint64_t text = 0;
    std::uint64_t rodata = 0;
    std::uint64_t data = 0;
    std::uint64_t bss = 0;
};

class RcuObjectLayout {
public:
    [[nodiscard]] static RcuObjectLayout Build(std::span<const RcuFile> objects,
                                               const RcuLayoutPrefixes &prefixes = {});

    [[nodiscard]] const std::vector<std::uint8_t> &Data(RcuMergedSection section) const;

    [[nodiscard]] std::uint64_t BssSize() const noexcept {
        return bssSize;
    }

    [[nodiscard]] std::uint16_t Alignment(RcuMergedSection section) const;

    [[nodiscard]] std::optional<RcuSectionPlacement> Section(std::size_t objectIndex, std::size_t sectionIndex) const;
    [[nodiscard]] std::optional<RcuSectionPlacement> Symbol(RcuSymbolLocation location) const;
    [[nodiscard]] std::optional<RcuSectionPlacement> Relocation(std::size_t objectIndex, std::size_t sectionIndex,
                                                                std::size_t relocationIndex) const;

    [[nodiscard]] std::optional<RcuSectionPlacement> Relocation(const RcuLinkReference &reference) const {
        return Relocation(reference.objectIndex, reference.sectionIndex, reference.relocationIndex);
    }

    [[nodiscard]] static std::optional<std::uint64_t> Address(RcuSectionPlacement placement,
                                                              const RcuSectionBases &bases);

private:
    struct ObjectPlacement {
        std::vector<std::optional<RcuSectionPlacement>> sections;
        std::vector<std::optional<RcuSectionPlacement>> symbols;
        std::vector<std::vector<std::optional<RcuSectionPlacement>>> relocations;
    };

    std::array<std::vector<std::uint8_t>, 3> data;
    std::array<std::uint16_t, 4> alignments = {1, 1, 1, 1};
    std::vector<ObjectPlacement> objects;
    std::uint64_t bssSize = 0;
};
} // namespace Rux
