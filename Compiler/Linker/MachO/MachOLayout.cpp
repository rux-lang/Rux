// Chooses every address, file offset and load command of a Mach-O image. All of
// it is checked arithmetic: a wrapped size would produce an image that overlaps
// itself rather than a diagnostic.

#include "Linker/MachO/MachOLayout.h"

#include <algorithm>
#include <limits>
#include <optional>

namespace Rux::MachO {
namespace {
constexpr std::uint32_t kSegmentCommandSize = 72;
constexpr std::uint32_t kSectionSize = 80;
constexpr std::uint32_t kMachHeaderSize = 32;
constexpr std::size_t kMachUuidSize = 16;
constexpr std::string_view kDyldPath = "/usr/lib/dyld";

/// Round up to an alignment boundary, reporting overflow rather than wrapping. Layout arithmetic runs on attacker- or
/// bug-supplied sizes, so a wrapped address would silently produce an image that overlaps itself.
std::optional<std::uint64_t> AlignUp64(const std::uint64_t value, const std::uint64_t alignment) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0 ||
        value > std::numeric_limits<std::uint64_t>::max() - (alignment - 1)) {
        return std::nullopt;
    }
    return (value + alignment - 1) & ~(alignment - 1);
}

/// Add, or report that the sum does not fit. Same reasoning as `AlignUp64`.
std::optional<std::uint64_t> CheckedAdd(const std::uint64_t left, const std::uint64_t right) {
    if (left > std::numeric_limits<std::uint64_t>::max() - right) {
        return std::nullopt;
    }
    return left + right;
}

/// The padded size of a load command that carries a string, such as a library path.
std::optional<std::uint64_t> StringCommandSize(const std::uint32_t headerSize, const std::string_view value) {
    const auto unaligned = CheckedAdd(headerSize + 1, value.size());
    return unaligned ? AlignUp64(*unaligned, 8) : std::nullopt;
}

/// Write a fixed-width, NUL-padded name field, truncating rather than overflowing the field.
void WriteMachName(Buf &buffer, const std::string_view name) {
    for (std::size_t index = 0; index < 16; ++index) {
        WriteU8(buffer, index < name.size() ? static_cast<std::uint8_t>(name[index]) : 0);
    }
}

void WriteStringCommand(Buf &commands, const std::uint32_t command, const std::uint32_t headerSize,
                        const std::string_view value, const std::uint32_t timestamp) {
    const auto commandSize = StringCommandSize(headerSize, value);
    WriteU32(commands, command);
    WriteU32(commands, static_cast<std::uint32_t>(*commandSize));
    WriteU32(commands, headerSize);
    if (headerSize == 24) {
        WriteU32(commands, timestamp);
        WriteU32(commands, 0x10000); // current version 1.0.0
        WriteU32(commands, 0x10000); // compatibility version 1.0.0
    }
    for (const char byte : value) {
        WriteU8(commands, static_cast<std::uint8_t>(byte));
    }
    WriteU8(commands, 0);
    while (commands.size() % 8 != 0) {
        WriteU8(commands, 0);
    }
}

/// Convert an alignment in bytes to the log2 form Mach-O section headers store.
std::uint32_t AlignmentPower(const std::uint32_t alignment) {
    std::uint32_t power = 0;
    for (std::uint32_t value = alignment; value > 1; value >>= 1U) {
        ++power;
    }
    return power;
}

void WriteSection(Buf &commands, const std::string_view sectionName, const std::string_view segmentName,
                  const std::uint64_t address, const std::uint64_t size, const std::uint32_t offset,
                  const std::uint32_t alignment, const std::uint32_t flags, const std::uint32_t reserved1 = 0,
                  const std::uint32_t reserved2 = 0) {
    WriteMachName(commands, sectionName);
    WriteMachName(commands, segmentName);
    WriteU64(commands, address);
    WriteU64(commands, size);
    WriteU32(commands, offset);
    WriteU32(commands, alignment);
    WriteU32(commands, 0);
    WriteU32(commands, 0);
    WriteU32(commands, flags);
    WriteU32(commands, reserved1);
    WriteU32(commands, reserved2);
    WriteU32(commands, 0);
}

void WriteSegment(Buf &commands, const std::uint32_t commandSize, const std::string_view name,
                  const std::uint64_t address, const std::uint64_t vmSize, const std::uint64_t fileOffset,
                  const std::uint64_t fileSize, const std::uint32_t maximumProtection,
                  const std::uint32_t initialProtection, const std::uint32_t sectionCount,
                  const std::uint32_t flags = 0) {
    WriteU32(commands, 0x19); // LC_SEGMENT_64
    WriteU32(commands, commandSize);
    WriteMachName(commands, name);
    WriteU64(commands, address);
    WriteU64(commands, vmSize);
    WriteU64(commands, fileOffset);
    WriteU64(commands, fileSize);
    WriteU32(commands, maximumProtection);
    WriteU32(commands, initialProtection);
    WriteU32(commands, sectionCount);
    WriteU32(commands, flags);
}
} // namespace

ImageLayout ImageLayoutBuilder::Plan(const ImageLayoutRequest &request) {
    ImageLayout result;
    result.textSectionCount = (request.dynamic ? 2U : 1U) + (request.writableConstantSegment ? 0U : 1U);
    result.dataSectionCount = request.dynamic ? 2U : 1U;
    result.textSegmentIndex = request.shared ? 0 : 1;
    result.constantSegmentIndex =
        static_cast<std::uint8_t>(result.textSegmentIndex + (request.writableConstantSegment ? 1 : 0));
    result.dataSegmentIndex = static_cast<std::uint8_t>(result.constantSegmentIndex + 1);

    const std::uint64_t textCommandSize = kSegmentCommandSize + result.textSectionCount * kSectionSize;
    const std::uint64_t constantCommandSize = kSegmentCommandSize + kSectionSize;
    const std::uint64_t dataCommandSize = kSegmentCommandSize + result.dataSectionCount * kSectionSize;
    std::uint64_t commandCount = request.shared ? 3 : 4;
    std::uint64_t commandsSize = textCommandSize + dataCommandSize + kSegmentCommandSize;
    if (!request.shared) {
        commandsSize += kSegmentCommandSize;
    }
    if (request.writableConstantSegment) {
        ++commandCount;
        commandsSize += constantCommandSize;
    }
    if (request.dynamic) {
        commandCount += (request.shared ? 4U : 5U) + request.neededLibraries.size();
        commandsSize += 48 + 24 + 80;
        const auto imageCommandSize =
            request.shared ? StringCommandSize(24, request.installName) : StringCommandSize(12, kDyldPath);
        if (!imageCommandSize) {
            result.diagnostics.emplace_back("Mach-O image load-command size overflows the image layout");
            return result;
        }
        commandsSize += *imageCommandSize + (request.shared ? 0 : 24);
        for (const std::string &library : request.neededLibraries) {
            const auto libraryCommandSize = StringCommandSize(24, library);
            if (!libraryCommandSize) {
                result.diagnostics.emplace_back("Mach-O library load-command size overflows the image layout");
                return result;
            }
            commandsSize += *libraryCommandSize;
        }
    }
    else {
        ++commandCount;
        commandsSize += 16 + static_cast<std::uint64_t>(request.threadStateCount) * 4;
    }
    ++commandCount;
    commandsSize += 24; // LC_UUID
    if (request.emitBuildVersion) {
        ++commandCount;
        commandsSize += 24;
    }
    ++commandCount;
    commandsSize += 16; // LC_CODE_SIGNATURE
    if (commandCount > std::numeric_limits<std::uint32_t>::max() ||
        commandsSize > std::numeric_limits<std::uint32_t>::max()) {
        result.diagnostics.emplace_back("Mach-O load-command table does not fit in its 32-bit header fields");
        return result;
    }
    result.commandCount = static_cast<std::uint32_t>(commandCount);
    result.commandsSize = static_cast<std::uint32_t>(commandsSize);

    const auto align = [&](const std::uint64_t value, const std::uint64_t alignment,
                           const std::string_view description) {
        const auto aligned = AlignUp64(value, alignment);
        if (!aligned) {
            result.diagnostics.push_back("Mach-O " + std::string(description) +
                                         " alignment overflows the image layout");
        }
        return aligned;
    };
    const auto add = [&](const std::uint64_t left, const std::uint64_t right, const std::string_view description) {
        const auto sum = CheckedAdd(left, right);
        if (!sum) {
            result.diagnostics.push_back("Mach-O " + std::string(description) + " overflows the image layout");
        }
        return sum;
    };

    const auto textOffset = align(kMachHeaderSize + commandsSize, request.fileAlignment, "text offset");
    const auto textEnd = textOffset ? add(*textOffset, request.textSize, "text size") : std::nullopt;
    const auto stubOffset =
        textEnd ? align(*textEnd, request.instructionStubAlignment, "instruction-stub offset") : std::nullopt;
    const auto stubEnd = stubOffset ? add(*stubOffset, request.stubSize, "instruction-stub size") : std::nullopt;
    const std::uint64_t constantAlignment = request.writableConstantSegment ? request.vmPageAlignment : 16;
    const auto constantDataOffset = stubEnd ? align(*stubEnd, constantAlignment, "constant-data offset") : std::nullopt;
    const auto textSegmentFileEnd =
        request.writableConstantSegment
            ? stubEnd
            : (constantDataOffset ? add(*constantDataOffset, request.constantDataSize, "text segment size")
                                  : std::nullopt);
    const auto textSegmentVMSize =
        textSegmentFileEnd ? align(*textSegmentFileEnd, request.vmPageAlignment, "text segment") : std::nullopt;
    const auto constantSegmentVMSize =
        request.writableConstantSegment
            ? align(std::max<std::uint64_t>(request.constantDataSize, 1), request.vmPageAlignment, "constant segment")
            : std::optional<std::uint64_t>{0};
    const auto dataSegmentOffset =
        request.writableConstantSegment
            ? (constantDataOffset && constantSegmentVMSize
                   ? add(*constantDataOffset, *constantSegmentVMSize, "data segment")
                   : std::nullopt)
            : (textSegmentFileEnd ? align(*textSegmentFileEnd, request.vmPageAlignment, "data segment") : std::nullopt);
    const auto pointerEnd =
        dataSegmentOffset ? add(*dataSegmentOffset, request.importPointerSize, "import-pointer size") : std::nullopt;
    const auto dataOffset = pointerEnd ? align(*pointerEnd, 8, "writable-data offset") : std::nullopt;
    const auto dataEnd = dataOffset ? add(*dataOffset, request.writableDataSize, "writable-data size") : std::nullopt;
    const auto dataSegmentVMSize =
        dataSegmentOffset && dataEnd
            ? align(std::max<std::uint64_t>(*dataEnd - *dataSegmentOffset, 1), request.vmPageAlignment, "data segment")
            : std::nullopt;
    const auto linkEditOffset = dataSegmentOffset && dataSegmentVMSize
                                  ? add(*dataSegmentOffset, *dataSegmentVMSize, "link-edit offset")
                                  : std::nullopt;
    if (result.HasErrors() || !textOffset || !stubOffset || !constantDataOffset || !textSegmentFileEnd ||
        !textSegmentVMSize || !constantSegmentVMSize || !dataSegmentOffset || !dataOffset || !dataEnd ||
        !dataSegmentVMSize || !linkEditOffset) {
        return result;
    }

    const auto textAddress = add(request.imageBase, *textOffset, "text address");
    const auto stubAddress = add(request.imageBase, *stubOffset, "instruction-stub address");
    const auto constantDataAddress = add(request.imageBase, *constantDataOffset, "constant-data address");
    const auto dataSegmentAddress = add(request.imageBase, *dataSegmentOffset, "data-segment address");
    const auto writableDataAddress = add(request.imageBase, *dataOffset, "writable-data address");
    const auto linkEditAddress = add(request.imageBase, *linkEditOffset, "link-edit address");
    if (result.HasErrors() || !textAddress || !stubAddress || !constantDataAddress || !dataSegmentAddress ||
        !writableDataAddress || !linkEditAddress) {
        return result;
    }

    result.textOffset = *textOffset;
    result.stubOffset = *stubOffset;
    result.constantDataOffset = *constantDataOffset;
    result.importPointerOffset = *dataSegmentOffset;
    result.writableDataOffset = *dataOffset;
    result.linkEditOffset = *linkEditOffset;
    result.textAddress = *textAddress;
    result.stubAddress = *stubAddress;
    result.constantDataAddress = *constantDataAddress;
    result.dataSegmentAddress = *dataSegmentAddress;
    result.importPointerAddress = result.dataSegmentAddress;
    result.constantSegmentAddress = request.writableConstantSegment ? result.constantDataAddress : request.imageBase;
    result.linkEditAddress = *linkEditAddress;
    result.textSegmentFileEnd = *textSegmentFileEnd;
    result.textSegmentVMSize = *textSegmentVMSize;
    result.constantSegmentFileSize = request.writableConstantSegment ? request.constantDataSize : 0;
    result.constantSegmentVMSize = *constantSegmentVMSize;
    result.dataSegmentFileSize = *dataEnd - *dataSegmentOffset;
    result.dataSegmentVMSize = *dataSegmentVMSize;
    result.sectionBases = {
        .text = result.textAddress,
        .rodata = result.constantDataAddress,
        .data = *writableDataAddress,
    };

    const auto requireU32 = [&](const std::uint64_t value, const std::string_view description) {
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            result.diagnostics.push_back("Mach-O " + std::string(description) +
                                         " does not fit in its 32-bit load-command field");
        }
    };
    requireU32(result.textOffset, "text file offset");
    requireU32(result.stubOffset, "instruction-stub file offset");
    requireU32(result.constantDataOffset, "constant-data file offset");
    requireU32(result.importPointerOffset, "import-pointer file offset");
    requireU32(result.writableDataOffset, "writable-data file offset");
    requireU32(result.linkEditOffset, "link-edit file offset");
    requireU32(request.importCount, "import count");
    return result;
}

LoadCommandLayout ImageLayoutBuilder::BuildLoadCommands(const ImageLayoutRequest &request, const ImageLayout &layout,
                                                        const LinkEditLayout &linkEdit) {
    LoadCommandLayout result;
    const std::uint32_t textCommandSize = kSegmentCommandSize + layout.textSectionCount * kSectionSize;
    const std::uint32_t constantCommandSize = kSegmentCommandSize + kSectionSize;
    const std::uint32_t dataCommandSize = kSegmentCommandSize + layout.dataSectionCount * kSectionSize;
    if (!request.shared) {
        WriteSegment(result.commands, kSegmentCommandSize, "__PAGEZERO", 0, request.imageBase, 0, 0, 0, 0, 0);
    }

    WriteSegment(result.commands, textCommandSize, "__TEXT", request.imageBase, layout.textSegmentVMSize, 0,
                 layout.textSegmentFileEnd, 0x05, 0x05, layout.textSectionCount);
    WriteSection(result.commands, "__text", "__TEXT", layout.textAddress, request.textSize,
                 static_cast<std::uint32_t>(layout.textOffset), 4, 0x8000'0400);
    if (request.dynamic) {
        WriteSection(result.commands, "__stubs", "__TEXT", layout.stubAddress, request.stubSize,
                     static_cast<std::uint32_t>(layout.stubOffset), AlignmentPower(request.instructionStubAlignment),
                     0x8000'0408, 0, request.instructionStubSize);
    }

    const std::string_view constantSegmentName = request.writableConstantSegment ? "__DATA_CONST" : "__TEXT";
    if (request.writableConstantSegment) {
        WriteSegment(result.commands, constantCommandSize, constantSegmentName, layout.constantSegmentAddress,
                     layout.constantSegmentVMSize, layout.constantDataOffset, layout.constantSegmentFileSize, 0x03,
                     0x03, 1, 0x10);
    }
    WriteSection(result.commands, "__const", constantSegmentName, layout.constantDataAddress, request.constantDataSize,
                 static_cast<std::uint32_t>(layout.constantDataOffset), 4, 0);

    WriteSegment(result.commands, dataCommandSize, "__DATA", layout.dataSegmentAddress, layout.dataSegmentVMSize,
                 layout.importPointerOffset, layout.dataSegmentFileSize, 0x03, 0x03, layout.dataSectionCount);
    if (request.dynamic) {
        WriteSection(result.commands, "__nl_symbol_ptr", "__DATA", layout.importPointerAddress,
                     request.importPointerSize, static_cast<std::uint32_t>(layout.importPointerOffset), 3, 0x06,
                     static_cast<std::uint32_t>(request.importCount));
    }
    WriteSection(result.commands, "__data", "__DATA", layout.sectionBases.data, request.writableDataSize,
                 static_cast<std::uint32_t>(layout.writableDataOffset), 0, 0);

    WriteSegment(result.commands, kSegmentCommandSize, "__LINKEDIT", layout.linkEditAddress, linkEdit.vmSize,
                 layout.linkEditOffset, linkEdit.fileSize, 0x01, 0x01, 0);
    if (request.dynamic) {
        WriteU32(result.commands, 0x8000'0022); // LC_DYLD_INFO_ONLY
        WriteU32(result.commands, 48);
        WriteU32(result.commands,
                 linkEdit.rebaseOpcodes.empty() ? 0 : static_cast<std::uint32_t>(layout.linkEditOffset));
        WriteU32(result.commands, static_cast<std::uint32_t>(linkEdit.rebaseOpcodes.size()));
        WriteU32(result.commands, linkEdit.bindOpcodes.empty() ? 0 : static_cast<std::uint32_t>(linkEdit.bindOffset));
        WriteU32(result.commands, static_cast<std::uint32_t>(linkEdit.bindOpcodes.size()));
        WriteU32(result.commands, 0);
        WriteU32(result.commands, 0);
        WriteU32(result.commands, 0);
        WriteU32(result.commands, 0);
        WriteU32(result.commands,
                 linkEdit.exportTrie.empty() ? 0 : static_cast<std::uint32_t>(linkEdit.exportTrieOffset));
        WriteU32(result.commands, static_cast<std::uint32_t>(linkEdit.exportTrie.size()));

        WriteU32(result.commands, 0x02); // LC_SYMTAB
        WriteU32(result.commands, 24);
        WriteU32(result.commands, static_cast<std::uint32_t>(linkEdit.symbolTableOffset));
        WriteU32(result.commands, linkEdit.symbolCount);
        WriteU32(result.commands, static_cast<std::uint32_t>(linkEdit.stringTableOffset));
        WriteU32(result.commands, linkEdit.stringTableSize);

        WriteU32(result.commands, 0x0B); // LC_DYSYMTAB
        WriteU32(result.commands, 80);
        WriteU32(result.commands, 0);
        WriteU32(result.commands, 0);
        WriteU32(result.commands, 0);
        WriteU32(result.commands, linkEdit.exportCount);
        WriteU32(result.commands, linkEdit.exportCount);
        WriteU32(result.commands, linkEdit.importCount);
        for (std::size_t index = 0; index < 6; ++index) {
            WriteU32(result.commands, 0);
        }
        WriteU32(result.commands, static_cast<std::uint32_t>(linkEdit.indirectSymbolsOffset));
        WriteU32(result.commands, linkEdit.importCount * 2);
        for (std::size_t index = 0; index < 4; ++index) {
            WriteU32(result.commands, 0);
        }

        if (request.shared) {
            WriteStringCommand(result.commands, 0x0D, 24, request.installName, 0);
        }
        else {
            WriteStringCommand(result.commands, 0x0E, 12, kDyldPath, 0);
            WriteU32(result.commands, 0x8000'0028); // LC_MAIN
            WriteU32(result.commands, 24);
            WriteU64(result.commands, layout.textOffset);
            WriteU64(result.commands, 0);
        }
        for (const std::string &library : request.neededLibraries) {
            WriteStringCommand(result.commands, 0x0C, 24, library, 2);
        }
    }
    else {
        const std::uint32_t threadCommandSize = 16 + request.threadStateCount * 4;
        WriteU32(result.commands, 0x05); // LC_UNIXTHREAD
        WriteU32(result.commands, threadCommandSize);
        WriteU32(result.commands, request.threadStateFlavor);
        WriteU32(result.commands, request.threadStateCount);
        for (std::uint32_t reg = 0; reg < request.threadStateCount / 2; ++reg) {
            WriteU64(result.commands, reg == request.threadProgramCounterIndex ? layout.textAddress : 0);
        }
    }

    WriteU32(result.commands, 0x1B); // LC_UUID
    WriteU32(result.commands, 24);
    const std::size_t uuidCommandPayload = result.commands.size();
    result.commands.insert(result.commands.end(), kMachUuidSize, 0);
    if (request.emitBuildVersion) {
        WriteU32(result.commands, 0x32); // LC_BUILD_VERSION
        WriteU32(result.commands, 24);
        WriteU32(result.commands, 1);
        WriteU32(result.commands, 0x001A'0000);
        WriteU32(result.commands, 0x001A'0000);
        WriteU32(result.commands, 0);
    }
    WriteU32(result.commands, 0x1D); // LC_CODE_SIGNATURE
    WriteU32(result.commands, 16);
    WriteU32(result.commands, static_cast<std::uint32_t>(linkEdit.codeSignatureOffset));
    WriteU32(result.commands, static_cast<std::uint32_t>(linkEdit.codeSignatureSize));
    if (result.commands.size() != layout.commandsSize) {
        result.diagnostics.emplace_back("internal: Mach-O load-command size mismatch");
        return result;
    }

    WriteU32(result.header, 0xFEED'FACF); // MH_MAGIC_64
    WriteU32(result.header, request.cpuType);
    WriteU32(result.header, request.cpuSubtype);
    WriteU32(result.header, request.shared ? 6 : 2);
    WriteU32(result.header, layout.commandCount);
    WriteU32(result.header, layout.commandsSize);
    WriteU32(result.header,
             (request.dynamic ? 0x0000'0005U : 0x0000'0001U) | (request.positionIndependent ? 0x0020'0000U : 0U));
    WriteU32(result.header, 0);
    result.uuidOffset = result.header.size() + uuidCommandPayload;
    return result;
}
} // namespace Rux::MachO
