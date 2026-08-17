#pragma once

#include "Linker/LinkerInternal.h"
#include "Linker/RcuLinkGraph.h"
#include "Linker/RcuObjectLayout.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Rux::MachO {
/// One symbol imported from another library, with the ordinal identifying which one. Mach-O encodes the library as a
/// small index into the load commands rather than by name, so the ordinal has to be assigned before binding is written.
struct LinkEditImport {
    std::string name;
    std::uint8_t libraryOrdinal = 0;
};

/// The addresses LINKEDIT contents are expressed relative to. Binding and relocation entries store offsets from a
/// segment base, so building them needs the layout to have been decided first.
struct LinkEditSegmentLayout {
    RcuSectionBases sectionBases;
    std::uint64_t imageBase = 0;
    std::uint64_t constantSegmentAddress = 0;
    std::uint64_t dataSegmentAddress = 0;
    std::uint64_t linkEditOffset = 0;
    std::uint64_t vmPageAlignment = 0;
    std::uint8_t textSegmentIndex = 0;
    std::uint8_t constantSegmentIndex = 0;
    std::uint8_t dataSegmentIndex = 0;
};

/// The inputs for building the LINKEDIT segment: the symbol tables, binding opcodes, and relocation entries the dynamic
/// loader reads. It is built last because every offset in it points into a placed image.
struct LinkEditBuildRequest {
    const RcuLinkGraph &graph;
    std::span<const RcuFile> objects;
    const RcuObjectLayout &objectLayout;
    std::span<const LinkEditImport> imports;
    LinkEditSegmentLayout segments;
    std::string_view codeSignatureIdentifier;
    bool dynamic = false;
    bool shared = false;
    bool slid = false;
    bool writableConstantSegment = false;
};

struct LinkEditLayout {
    Buf contents;
    Buf rebaseOpcodes;
    Buf bindOpcodes;
    Buf exportTrie;
    std::uint64_t bindOffset = 0;
    std::uint64_t exportTrieOffset = 0;
    std::uint64_t symbolTableOffset = 0;
    std::uint64_t indirectSymbolsOffset = 0;
    std::uint64_t stringTableOffset = 0;
    std::uint64_t codeSignatureOffset = 0;
    std::uint64_t codeSignatureSize = 0;
    std::uint64_t fileSize = 0;
    std::uint64_t vmSize = 0;
    std::uint32_t exportCount = 0;
    std::uint32_t importCount = 0;
    std::uint32_t symbolCount = 0;
    std::uint32_t stringTableSize = 0;
    std::vector<std::string> diagnostics;

    [[nodiscard]] bool HasErrors() const noexcept {
        return !diagnostics.empty();
    }
};

class LinkEditBuilder {
public:
    [[nodiscard]] static LinkEditLayout Build(const LinkEditBuildRequest &request);
};
} // namespace Rux::MachO
