#include "Linker/MachO/MachOLinkEdit.h"

#include "Linker/MachO/CodeSignature.h"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>

namespace Rux::MachO {
namespace {
void WriteUleb128(Buf &buffer, std::uint64_t value) {
    do {
        std::uint8_t byte = static_cast<std::uint8_t>(value & 0x7F);
        value >>= 7;
        if (value != 0) {
            byte |= 0x80;
        }
        WriteU8(buffer, byte);
    }
    while (value != 0);
}

std::optional<std::uint64_t> AlignUp64(const std::uint64_t value, const std::uint64_t alignment) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0 ||
        value > std::numeric_limits<std::uint64_t>::max() - (alignment - 1)) {
        return std::nullopt;
    }
    return (value + alignment - 1) & ~(alignment - 1);
}

std::optional<std::uint64_t> CheckedAdd(const std::uint64_t left, const std::uint64_t right) {
    if (left > std::numeric_limits<std::uint64_t>::max() - right) {
        return std::nullopt;
    }
    return left + right;
}

Buf BuildExportTrie(const std::vector<std::pair<std::string, std::uint64_t>> &exports) {
    if (exports.empty()) {
        return {};
    }

    struct TrieNode {
        std::map<std::uint8_t, std::size_t> children;
        bool terminal = false;
        std::uint64_t address = 0;
    };

    std::vector<TrieNode> nodes(1);
    for (const auto &[name, address] : exports) {
        std::size_t nodeIndex = 0;
        const std::string nativeName = "_" + name;
        for (const unsigned char character : nativeName) {
            const auto child = nodes[nodeIndex].children.find(character);
            if (child != nodes[nodeIndex].children.end()) {
                nodeIndex = child->second;
                continue;
            }
            const std::size_t childIndex = nodes.size();
            nodes.push_back({});
            nodes[nodeIndex].children.emplace(character, childIndex);
            nodeIndex = childIndex;
        }
        nodes[nodeIndex].terminal = true;
        nodes[nodeIndex].address = address;
    }

    std::vector<std::size_t> offsets(nodes.size());
    std::vector<Buf> encoded(nodes.size());
    for (;;) {
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            encoded[i].clear();
            if (nodes[i].terminal) {
                Buf terminal;
                WriteUleb128(terminal, 0);
                WriteUleb128(terminal, nodes[i].address);
                WriteUleb128(encoded[i], terminal.size());
                encoded[i].insert(encoded[i].end(), terminal.begin(), terminal.end());
            }
            else {
                WriteU8(encoded[i], 0);
            }
            WriteU8(encoded[i], static_cast<std::uint8_t>(nodes[i].children.size()));
            for (const auto &[character, childIndex] : nodes[i].children) {
                WriteU8(encoded[i], character);
                WriteU8(encoded[i], 0);
                WriteUleb128(encoded[i], offsets[childIndex]);
            }
        }

        std::vector<std::size_t> nextOffsets(nodes.size());
        std::size_t cursor = 0;
        for (std::size_t i = 0; i < encoded.size(); ++i) {
            nextOffsets[i] = cursor;
            cursor += encoded[i].size();
        }
        if (nextOffsets == offsets) {
            break;
        }
        offsets = std::move(nextOffsets);
    }

    Buf trie;
    for (const auto &node : encoded) {
        trie.insert(trie.end(), node.begin(), node.end());
    }
    return trie;
}

std::uint8_t SectionOrdinal(const RcuMergedSection section) {
    switch (section) {
    case RcuMergedSection::Text:
        return 1;
    case RcuMergedSection::RoData:
        return 3;
    case RcuMergedSection::Data:
        return 5;
    case RcuMergedSection::Bss:
        return 0;
    }
    return 0;
}

void AddNativeString(Buf &table, const std::string_view name) {
    WriteU8(table, '_');
    for (const char byte : name) {
        WriteU8(table, static_cast<std::uint8_t>(byte));
    }
    WriteU8(table, 0);
}
} // namespace

LinkEditLayout LinkEditBuilder::Build(const LinkEditBuildRequest &request) {
    LinkEditLayout result;
    const LinkEditSegmentLayout &segments = request.segments;

    if (request.dynamic && !request.imports.empty()) {
        WriteU8(result.bindOpcodes, 0x51); // SET_TYPE_IMM | POINTER
        WriteU8(result.bindOpcodes,
                static_cast<std::uint8_t>(0x70 | segments.dataSegmentIndex)); // SET_SEGMENT_AND_OFFSET_ULEB
        WriteUleb128(result.bindOpcodes, 0);
        for (const LinkEditImport &import : request.imports) {
            if (import.libraryOrdinal <= 15) {
                WriteU8(result.bindOpcodes,
                        static_cast<std::uint8_t>(0x10 | import.libraryOrdinal)); // SET_DYLIB_ORDINAL_IMM
            }
            else {
                WriteU8(result.bindOpcodes, 0x20); // SET_DYLIB_ORDINAL_ULEB
                WriteUleb128(result.bindOpcodes, import.libraryOrdinal);
            }
            WriteU8(result.bindOpcodes, 0x40); // SET_SYMBOL_TRAILING_FLAGS_IMM
            AddNativeString(result.bindOpcodes, import.name);
            WriteU8(result.bindOpcodes, 0x90); // DO_BIND
        }
        WriteU8(result.bindOpcodes, 0); // DONE
    }

    if (request.slid) {
        WriteU8(result.rebaseOpcodes, 0x11); // SET_TYPE_IMM | REBASE_TYPE_POINTER
        for (const RcuLinkReference &reference : request.graph.References()) {
            const RcuFile &object = request.objects[reference.objectIndex];
            const RcuReloc &relocation = object.sections[reference.sectionIndex].relocs[reference.relocationIndex];
            if (relocation.type != RcuRelType::Abs64) {
                continue;
            }
            const auto placement = request.objectLayout.Relocation(reference);
            const auto address = placement ? RcuObjectLayout::Address(*placement, segments.sectionBases) : std::nullopt;
            if (!placement || !address || placement->section == RcuMergedSection::Bss) {
                continue;
            }
            if (request.writableConstantSegment && placement->section == RcuMergedSection::Text) {
                const RcuSymbol &symbol = object.symbols[reference.symbolIndex];
                result.diagnostics.push_back("Mach-O code cannot hold an absolute address in a position-independent "
                                             "image; '" +
                                             symbol.name + "' must be reached PC-relatively");
                continue;
            }

            std::uint8_t segmentIndex = 0;
            std::uint64_t segmentOffset = 0;
            if (placement->section == RcuMergedSection::Text) {
                segmentIndex = segments.textSegmentIndex;
                segmentOffset = *address - segments.imageBase;
            }
            else if (placement->section == RcuMergedSection::RoData) {
                segmentIndex = segments.constantSegmentIndex;
                segmentOffset = *address - segments.constantSegmentAddress;
            }
            else {
                segmentIndex = segments.dataSegmentIndex;
                segmentOffset = *address - segments.dataSegmentAddress;
            }
            WriteU8(result.rebaseOpcodes, static_cast<std::uint8_t>(0x20 | segmentIndex));
            WriteUleb128(result.rebaseOpcodes, segmentOffset);
            WriteU8(result.rebaseOpcodes, 0x51); // DO_REBASE_IMM_TIMES | 1
        }
        WriteU8(result.rebaseOpcodes, 0); // DONE
    }

    Buf dynamicSymbols;
    Buf indirectSymbols;
    Buf stringTable = {0};
    const auto symbolAddress = [&](const RcuSymbolLocation location) -> std::optional<std::uint64_t> {
        const auto placement = request.objectLayout.Symbol(location);
        return placement && placement->section != RcuMergedSection::Bss
                 ? RcuObjectLayout::Address(*placement, segments.sectionBases)
                 : std::nullopt;
    };

    std::vector<std::pair<std::string, std::uint64_t>> exports;
    if (request.shared) {
        for (const RcuSymbolLocation source : request.graph.ExportRoots()) {
            const RcuSymbol &symbol = request.objects[source.objectIndex].symbols[source.symbolIndex];
            const auto address = symbolAddress(source);
            const auto placement = request.objectLayout.Symbol(source);
            if (!address || !placement) {
                continue;
            }
            const std::uint8_t sectionOrdinal = SectionOrdinal(placement->section);
            if (sectionOrdinal == 0) {
                continue;
            }

            const auto stringIndex = static_cast<std::uint32_t>(stringTable.size());
            AddNativeString(stringTable, symbol.name);
            WriteU32(dynamicSymbols, stringIndex);
            WriteU8(dynamicSymbols, 0x0F); // N_SECT | N_EXT
            WriteU8(dynamicSymbols, sectionOrdinal);
            WriteU16(dynamicSymbols, 0);
            WriteU64(dynamicSymbols, *address);
            exports.emplace_back(symbol.name, *address);
        }
    }

    for (const LinkEditImport &import : request.imports) {
        const auto stringIndex = static_cast<std::uint32_t>(stringTable.size());
        AddNativeString(stringTable, import.name);
        WriteU32(dynamicSymbols, stringIndex);
        WriteU8(dynamicSymbols, 0x01); // N_UNDF | N_EXT
        WriteU8(dynamicSymbols, 0);
        WriteU16(dynamicSymbols, static_cast<std::uint16_t>(import.libraryOrdinal) << 8);
        WriteU64(dynamicSymbols, 0);
    }

    if (request.dynamic) {
        for (std::size_t section = 0; section < 2; ++section) {
            for (std::size_t i = 0; i < request.imports.size(); ++i) {
                WriteU32(indirectSymbols, static_cast<std::uint32_t>(exports.size() + i));
            }
        }
    }

    result.exportTrie = BuildExportTrie(exports);
    result.contents = result.rebaseOpcodes;
    const auto bindOffset = CheckedAdd(segments.linkEditOffset, result.contents.size());
    if (!bindOffset) {
        result.diagnostics.emplace_back("Mach-O bind metadata offset overflows the image layout");
        return result;
    }
    result.bindOffset = *bindOffset;
    result.contents.insert(result.contents.end(), result.bindOpcodes.begin(), result.bindOpcodes.end());
    const auto exportTrieOffset = CheckedAdd(segments.linkEditOffset, result.contents.size());
    if (!exportTrieOffset) {
        result.diagnostics.emplace_back("Mach-O export metadata offset overflows the image layout");
        return result;
    }
    result.exportTrieOffset = *exportTrieOffset;
    result.contents.insert(result.contents.end(), result.exportTrie.begin(), result.exportTrie.end());
    while ((segments.linkEditOffset + result.contents.size()) % 8 != 0) {
        WriteU8(result.contents, 0);
    }
    result.symbolTableOffset = segments.linkEditOffset + result.contents.size();
    result.contents.insert(result.contents.end(), dynamicSymbols.begin(), dynamicSymbols.end());
    while ((segments.linkEditOffset + result.contents.size()) % 4 != 0) {
        WriteU8(result.contents, 0);
    }
    result.indirectSymbolsOffset = segments.linkEditOffset + result.contents.size();
    result.contents.insert(result.contents.end(), indirectSymbols.begin(), indirectSymbols.end());
    result.stringTableOffset = segments.linkEditOffset + result.contents.size();
    result.contents.insert(result.contents.end(), stringTable.begin(), stringTable.end());

    const auto unsignedEnd = CheckedAdd(segments.linkEditOffset, result.contents.size());
    const auto signatureOffset = unsignedEnd ? AlignUp64(*unsignedEnd, 16) : std::nullopt;
    if (!signatureOffset) {
        result.diagnostics.emplace_back("Mach-O code-signature offset alignment overflows the image layout");
        return result;
    }
    result.codeSignatureOffset = *signatureOffset;
    std::string signatureError;
    result.codeSignatureSize =
        AdHocCodeSignatureSize(result.codeSignatureOffset, request.codeSignatureIdentifier, signatureError);
    if (result.codeSignatureSize == 0) {
        result.diagnostics.push_back(std::move(signatureError));
        return result;
    }
    const auto signedFileEnd = CheckedAdd(result.codeSignatureOffset, result.codeSignatureSize);
    if (!signedFileEnd) {
        result.diagnostics.emplace_back("Mach-O code-signature size overflows the image layout");
        return result;
    }
    result.fileSize = *signedFileEnd - segments.linkEditOffset;
    const auto vmSize = AlignUp64(std::max<std::uint64_t>(result.fileSize, 1), segments.vmPageAlignment);
    if (!vmSize) {
        result.diagnostics.emplace_back("Mach-O link-edit segment alignment overflows the image layout");
        return result;
    }
    result.vmSize = *vmSize;

    const auto requireU32 = [&](const std::uint64_t value, const std::string_view description) {
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            result.diagnostics.push_back("Mach-O " + std::string(description) +
                                         " does not fit in its 32-bit load-command field");
            return false;
        }
        return true;
    };
    result.exportCount = static_cast<std::uint32_t>(exports.size());
    result.importCount = static_cast<std::uint32_t>(request.imports.size());
    result.symbolCount = static_cast<std::uint32_t>(dynamicSymbols.size() / 16);
    result.stringTableSize = static_cast<std::uint32_t>(stringTable.size());
    requireU32(result.bindOffset, "bind metadata offset");
    requireU32(result.exportTrieOffset, "export metadata offset");
    requireU32(result.symbolTableOffset, "symbol-table offset");
    requireU32(result.indirectSymbolsOffset, "indirect-symbol-table offset");
    requireU32(result.stringTableOffset, "string-table offset");
    requireU32(result.bindOpcodes.size(), "bind metadata size");
    requireU32(result.exportTrie.size(), "export metadata size");
    requireU32(dynamicSymbols.size() / 16, "symbol count");
    requireU32(stringTable.size(), "string-table size");
    requireU32(result.codeSignatureOffset, "code-signature offset");
    requireU32(result.codeSignatureSize, "code-signature size");
    return result;
}
} // namespace Rux::MachO
