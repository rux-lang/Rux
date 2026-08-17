#pragma once

#include "Linker/LinkerInternal.h"
#include "Linker/MachO/MachOLinkEdit.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Rux::MachO {
/// Everything image layout depends on, gathered before any address is chosen.
///
/// Sizes arrive already summed per section so the planner never walks the link graph: laying out an image is then
/// arithmetic over these numbers plus the alignment rules, which is what lets one implementation plan both an
/// executable and a shared library.
struct ImageLayoutRequest {
    std::span<const std::string> neededLibraries;
    std::string_view installName;
    std::uint64_t imageBase = 0;
    std::uint64_t vmPageAlignment = 0;
    std::uint32_t fileAlignment = 0;
    std::uint32_t instructionStubAlignment = 0;
    std::uint32_t instructionStubSize = 0;
    std::uint32_t cpuType = 0;
    std::uint32_t cpuSubtype = 0;
    std::uint32_t threadStateFlavor = 0;
    std::uint32_t threadStateCount = 0;
    std::uint32_t threadProgramCounterIndex = 0;
    std::uint64_t textSize = 0;
    std::uint64_t stubSize = 0;
    std::uint64_t constantDataSize = 0;
    std::uint64_t importPointerSize = 0;
    std::uint64_t writableDataSize = 0;
    std::size_t importCount = 0;
    bool dynamic = false;
    bool shared = false;
    bool positionIndependent = false;
    bool writableConstantSegment = false;
    bool emitBuildVersion = false;
};

struct ImageLayout {
    RcuSectionBases sectionBases;
    std::uint64_t textOffset = 0;
    std::uint64_t stubOffset = 0;
    std::uint64_t constantDataOffset = 0;
    std::uint64_t importPointerOffset = 0;
    std::uint64_t writableDataOffset = 0;
    std::uint64_t linkEditOffset = 0;
    std::uint64_t textAddress = 0;
    std::uint64_t stubAddress = 0;
    std::uint64_t constantDataAddress = 0;
    std::uint64_t importPointerAddress = 0;
    std::uint64_t constantSegmentAddress = 0;
    std::uint64_t dataSegmentAddress = 0;
    std::uint64_t linkEditAddress = 0;
    std::uint64_t textSegmentFileEnd = 0;
    std::uint64_t textSegmentVMSize = 0;
    std::uint64_t constantSegmentFileSize = 0;
    std::uint64_t constantSegmentVMSize = 0;
    std::uint64_t dataSegmentFileSize = 0;
    std::uint64_t dataSegmentVMSize = 0;
    std::uint32_t commandCount = 0;
    std::uint32_t commandsSize = 0;
    std::uint32_t textSectionCount = 0;
    std::uint32_t dataSectionCount = 0;
    std::uint8_t textSegmentIndex = 0;
    std::uint8_t constantSegmentIndex = 0;
    std::uint8_t dataSegmentIndex = 0;
    std::vector<std::string> diagnostics;

    [[nodiscard]] bool HasErrors() const noexcept {
        return !diagnostics.empty();
    }
};

struct LoadCommandLayout {
    Buf header;
    Buf commands;
    std::size_t uuidOffset = 0;
    std::vector<std::string> diagnostics;

    [[nodiscard]] bool HasErrors() const noexcept {
        return !diagnostics.empty();
    }
};

class ImageLayoutBuilder {
public:
    [[nodiscard]] static ImageLayout Plan(const ImageLayoutRequest &request);
    [[nodiscard]] static LoadCommandLayout BuildLoadCommands(const ImageLayoutRequest &request,
                                                             const ImageLayout &layout, const LinkEditLayout &linkEdit);
};
} // namespace Rux::MachO
