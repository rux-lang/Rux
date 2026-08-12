#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Rux::Testing {
enum class MachOArchitecture : std::uint8_t {
    Unknown,
    X86_64,
    AArch64,
};

struct MachOSection {
    std::string name;
    std::string segmentName;
    std::uint64_t address = 0;
    std::uint64_t size = 0;
    std::uint32_t offset = 0;
    std::uint32_t alignmentPower = 0;
    std::uint32_t flags = 0;
    std::uint32_t reserved1 = 0;
    std::uint32_t reserved2 = 0;
};

struct MachOSegment {
    std::string name;
    std::uint64_t vmAddress = 0;
    std::uint64_t vmSize = 0;
    std::uint64_t fileOffset = 0;
    std::uint64_t fileSize = 0;
    std::uint32_t maxProtection = 0;
    std::uint32_t initialProtection = 0;
    std::vector<MachOSection> sections;
};

struct MachOLoadCommand {
    std::uint32_t command = 0;
    std::uint32_t size = 0;
    std::size_t offset = 0;
    std::string value;
};

struct MachODyldInfo {
    std::uint32_t rebaseOffset = 0;
    std::uint32_t rebaseSize = 0;
    std::uint32_t bindOffset = 0;
    std::uint32_t bindSize = 0;
    std::uint32_t weakBindOffset = 0;
    std::uint32_t weakBindSize = 0;
    std::uint32_t lazyBindOffset = 0;
    std::uint32_t lazyBindSize = 0;
    std::uint32_t exportOffset = 0;
    std::uint32_t exportSize = 0;
};

struct MachOSymbol {
    std::string name;
    std::uint8_t type = 0;
    std::uint8_t section = 0;
    std::uint16_t description = 0;
    std::uint64_t value = 0;
};

struct MachOBind {
    std::uint8_t segmentIndex = 0;
    std::uint64_t segmentOffset = 0;
    std::uint32_t libraryOrdinal = 0;
    std::string symbol;
};

struct MachORebase {
    std::uint8_t segmentIndex = 0;
    std::uint64_t segmentOffset = 0;
};

struct MachORange {
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
};

struct MachOBuildVersion {
    std::uint32_t platform = 0;
    std::uint32_t minimumOs = 0;
    std::uint32_t sdk = 0;
    std::uint32_t toolCount = 0;
};

struct MachOCodeDirectory {
    std::uint32_t version = 0;
    std::uint32_t flags = 0;
    std::uint32_t codeLimit = 0;
    std::uint8_t hashSize = 0;
    std::uint8_t hashType = 0;
    std::uint8_t pageSizePower = 0;
    std::uint64_t executableSegmentBase = 0;
    std::uint64_t executableSegmentLimit = 0;
    std::uint64_t executableSegmentFlags = 0;
    std::string identifier;
    std::vector<std::vector<std::uint8_t>> codeHashes;
};

struct MachOImage {
    std::uint32_t cpuType = 0;
    std::uint32_t cpuSubtype = 0;
    std::uint32_t fileType = 0;
    std::uint32_t flags = 0;
    std::uint32_t declaredCommandCount = 0;
    std::uint32_t declaredCommandSize = 0;
    std::vector<MachOLoadCommand> commands;
    std::vector<MachOSegment> segments;
    std::vector<MachOSymbol> symbols;
    std::vector<std::uint32_t> indirectSymbols;
    std::vector<MachOBind> binds;
    std::vector<MachORebase> rebases;
    std::optional<MachODyldInfo> dyldInfo;
    std::optional<std::uint64_t> mainEntryOffset;
    std::optional<std::uint32_t> threadStateFlavor;
    std::optional<std::uint32_t> threadStateCount;
    std::optional<std::uint64_t> threadEntryAddress;
    std::optional<MachOBuildVersion> buildVersion;
    std::optional<MachORange> codeSignature;
    std::optional<MachOCodeDirectory> codeDirectory;

    [[nodiscard]] MachOArchitecture Architecture() const noexcept {
        if (cpuType == 0x0100'0007) {
            return MachOArchitecture::X86_64;
        }
        if (cpuType == 0x0100'000C) {
            return MachOArchitecture::AArch64;
        }
        return MachOArchitecture::Unknown;
    }

    [[nodiscard]] const MachOSegment *Segment(const std::string_view name) const {
        for (const auto &segment : segments) {
            if (segment.name == name) {
                return &segment;
            }
        }
        return nullptr;
    }

    [[nodiscard]] const MachOSection *Section(const std::string_view segmentName,
                                              const std::string_view sectionName) const {
        const auto *segment = Segment(segmentName);
        if (segment == nullptr) {
            return nullptr;
        }
        for (const auto &section : segment->sections) {
            if (section.name == sectionName) {
                return &section;
            }
        }
        return nullptr;
    }

    [[nodiscard]] std::vector<std::string> CommandValues(const std::uint32_t command) const {
        std::vector<std::string> values;
        for (const auto &loadCommand : commands) {
            if (loadCommand.command == command) {
                values.push_back(loadCommand.value);
            }
        }
        return values;
    }

    [[nodiscard]] bool HasCommand(const std::uint32_t command) const {
        for (const auto &loadCommand : commands) {
            if (loadCommand.command == command) {
                return true;
            }
        }
        return false;
    }
};

namespace Detail {
inline bool InBounds(const std::span<const std::uint8_t> bytes, const std::size_t offset, const std::size_t size) {
    return offset <= bytes.size() && size <= bytes.size() - offset;
}

inline std::uint16_t U16(const std::span<const std::uint8_t> bytes, const std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) | static_cast<std::uint16_t>(bytes[offset + 1]) << 8U;
}

inline std::uint32_t U32(const std::span<const std::uint8_t> bytes, const std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) | static_cast<std::uint32_t>(bytes[offset + 1]) << 8U |
           static_cast<std::uint32_t>(bytes[offset + 2]) << 16U | static_cast<std::uint32_t>(bytes[offset + 3]) << 24U;
}

inline std::uint64_t U64(const std::span<const std::uint8_t> bytes, const std::size_t offset) {
    return static_cast<std::uint64_t>(U32(bytes, offset)) | static_cast<std::uint64_t>(U32(bytes, offset + 4)) << 32U;
}

inline std::uint32_t BigU32(const std::span<const std::uint8_t> bytes, const std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) << 24U | static_cast<std::uint32_t>(bytes[offset + 1]) << 16U |
           static_cast<std::uint32_t>(bytes[offset + 2]) << 8U | static_cast<std::uint32_t>(bytes[offset + 3]);
}

inline std::uint64_t BigU64(const std::span<const std::uint8_t> bytes, const std::size_t offset) {
    return static_cast<std::uint64_t>(BigU32(bytes, offset)) << 32U | BigU32(bytes, offset + 4);
}

inline std::string FixedString(const std::span<const std::uint8_t> bytes, const std::size_t offset,
                               const std::size_t size) {
    std::size_t length = 0;
    while (length < size && bytes[offset + length] != 0) {
        ++length;
    }
    return {reinterpret_cast<const char *>(bytes.data() + offset), length};
}

inline std::optional<std::string> CommandString(const std::span<const std::uint8_t> bytes,
                                                const std::size_t commandOffset, const std::uint32_t commandSize,
                                                const std::uint32_t stringOffset) {
    if (stringOffset >= commandSize) {
        return std::nullopt;
    }
    const std::size_t begin = commandOffset + stringOffset;
    const std::size_t end = commandOffset + commandSize;
    std::size_t cursor = begin;
    while (cursor < end && bytes[cursor] != 0) {
        ++cursor;
    }
    if (cursor == end) {
        return std::nullopt;
    }
    return std::string(reinterpret_cast<const char *>(bytes.data() + begin), cursor - begin);
}

inline bool ReadUleb128(const std::span<const std::uint8_t> bytes, std::size_t &cursor, const std::size_t end,
                        std::uint64_t &value) {
    value = 0;
    unsigned shift = 0;
    while (cursor < end && shift < 64) {
        const std::uint8_t byte = bytes[cursor++];
        if (shift == 63 && (byte & 0x7E) != 0) {
            return false;
        }
        value |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            return true;
        }
        shift += 7;
    }
    return false;
}
} // namespace Detail

[[nodiscard]] inline bool ReadMachO64(const std::span<const std::uint8_t> bytes, MachOImage &image,
                                      std::string &error) {
    constexpr std::uint32_t kSegment64 = 0x19;
    constexpr std::uint32_t kSymtab = 0x02;
    constexpr std::uint32_t kDysymtab = 0x0B;
    constexpr std::uint32_t kUnixThread = 0x05;
    constexpr std::uint32_t kDyldInfoOnly = 0x8000'0022;
    constexpr std::uint32_t kMain = 0x8000'0028;
    constexpr std::uint32_t kBuildVersion = 0x32;
    constexpr std::uint32_t kCodeSignature = 0x1D;
    using namespace Detail;

    image = {};
    error.clear();
    if (!InBounds(bytes, 0, 32) || U32(bytes, 0) != 0xFEED'FACF) {
        error = "not a little-endian 64-bit Mach-O image";
        return false;
    }
    image.cpuType = U32(bytes, 4);
    image.cpuSubtype = U32(bytes, 8);
    image.fileType = U32(bytes, 12);
    image.declaredCommandCount = U32(bytes, 16);
    image.declaredCommandSize = U32(bytes, 20);
    image.flags = U32(bytes, 24);
    if (!InBounds(bytes, 32, image.declaredCommandSize)) {
        error = "Mach-O load-command area extends beyond the image";
        return false;
    }

    std::optional<std::uint32_t> symbolOffset;
    std::uint32_t symbolCount = 0;
    std::optional<std::uint32_t> stringOffset;
    std::uint32_t stringSize = 0;
    std::optional<std::uint32_t> indirectSymbolsOffset;
    std::uint32_t indirectSymbolCount = 0;
    std::size_t cursor = 32;
    const std::size_t commandEnd = cursor + image.declaredCommandSize;
    for (std::uint32_t index = 0; index < image.declaredCommandCount; ++index) {
        if (!InBounds(bytes, cursor, 8)) {
            error = "truncated Mach-O load-command header";
            return false;
        }
        const std::uint32_t command = U32(bytes, cursor);
        const std::uint32_t size = U32(bytes, cursor + 4);
        if (size < 8 || size > commandEnd - cursor) {
            error = "invalid Mach-O load-command size";
            return false;
        }
        MachOLoadCommand loadCommand{command, size, cursor, {}};
        if ((command == 0x0C || command == 0x0D) && size >= 24) {
            const auto value = CommandString(bytes, cursor, size, U32(bytes, cursor + 8));
            if (!value) {
                error = "invalid Mach-O dylib command string";
                return false;
            }
            loadCommand.value = *value;
        }
        else if (command == 0x0E && size >= 12) {
            const auto value = CommandString(bytes, cursor, size, U32(bytes, cursor + 8));
            if (!value) {
                error = "invalid Mach-O dylinker command string";
                return false;
            }
            loadCommand.value = *value;
        }
        image.commands.push_back(std::move(loadCommand));

        if (command == kSegment64) {
            if (size < 72) {
                error = "truncated Mach-O segment command";
                return false;
            }
            const std::uint32_t sectionCount = U32(bytes, cursor + 64);
            if (sectionCount > (size - 72) / 80) {
                error = "Mach-O segment sections extend beyond their command";
                return false;
            }
            MachOSegment segment;
            segment.name = FixedString(bytes, cursor + 8, 16);
            segment.vmAddress = U64(bytes, cursor + 24);
            segment.vmSize = U64(bytes, cursor + 32);
            segment.fileOffset = U64(bytes, cursor + 40);
            segment.fileSize = U64(bytes, cursor + 48);
            segment.maxProtection = U32(bytes, cursor + 56);
            segment.initialProtection = U32(bytes, cursor + 60);
            if (segment.fileSize > 0 &&
                (segment.fileOffset > bytes.size() || segment.fileSize > bytes.size() - segment.fileOffset)) {
                error = "Mach-O segment file range extends beyond the image";
                return false;
            }
            for (std::uint32_t sectionIndex = 0; sectionIndex < sectionCount; ++sectionIndex) {
                const std::size_t sectionOffset = cursor + 72 + static_cast<std::size_t>(sectionIndex) * 80;
                MachOSection section;
                section.name = FixedString(bytes, sectionOffset, 16);
                section.segmentName = FixedString(bytes, sectionOffset + 16, 16);
                section.address = U64(bytes, sectionOffset + 32);
                section.size = U64(bytes, sectionOffset + 40);
                section.offset = U32(bytes, sectionOffset + 48);
                section.alignmentPower = U32(bytes, sectionOffset + 52);
                section.flags = U32(bytes, sectionOffset + 64);
                section.reserved1 = U32(bytes, sectionOffset + 68);
                section.reserved2 = U32(bytes, sectionOffset + 72);
                segment.sections.push_back(std::move(section));
            }
            image.segments.push_back(std::move(segment));
        }
        else if (command == kMain) {
            if (size < 24) {
                error = "truncated Mach-O entry command";
                return false;
            }
            image.mainEntryOffset = U64(bytes, cursor + 8);
        }
        else if (command == kUnixThread) {
            if (size < 16) {
                error = "truncated Mach-O thread state";
                return false;
            }
            image.threadStateFlavor = U32(bytes, cursor + 8);
            image.threadStateCount = U32(bytes, cursor + 12);
            if (image.Architecture() == MachOArchitecture::X86_64) {
                if (size < 16 + 17 * 8) {
                    error = "truncated Mach-O x86-64 thread state";
                    return false;
                }
                image.threadEntryAddress = U64(bytes, cursor + 16 + 16 * 8);
            }
            else if (image.Architecture() == MachOArchitecture::AArch64) {
                if (size < 16 + 68 * 4) {
                    error = "truncated Mach-O AArch64 thread state";
                    return false;
                }
                image.threadEntryAddress = U64(bytes, cursor + 16 + 32 * 8);
            }
        }
        else if (command == kBuildVersion) {
            if (size < 24) {
                error = "truncated Mach-O build-version command";
                return false;
            }
            image.buildVersion = MachOBuildVersion{U32(bytes, cursor + 8), U32(bytes, cursor + 12),
                                                   U32(bytes, cursor + 16), U32(bytes, cursor + 20)};
        }
        else if (command == kDyldInfoOnly) {
            if (size < 48) {
                error = "truncated Mach-O dyld metadata command";
                return false;
            }
            image.dyldInfo = MachODyldInfo{U32(bytes, cursor + 8),  U32(bytes, cursor + 12), U32(bytes, cursor + 16),
                                           U32(bytes, cursor + 20), U32(bytes, cursor + 24), U32(bytes, cursor + 28),
                                           U32(bytes, cursor + 32), U32(bytes, cursor + 36), U32(bytes, cursor + 40),
                                           U32(bytes, cursor + 44)};
        }
        else if (command == kSymtab) {
            if (size < 24) {
                error = "truncated Mach-O symbol-table command";
                return false;
            }
            symbolOffset = U32(bytes, cursor + 8);
            symbolCount = U32(bytes, cursor + 12);
            stringOffset = U32(bytes, cursor + 16);
            stringSize = U32(bytes, cursor + 20);
        }
        else if (command == kDysymtab) {
            if (size < 80) {
                error = "truncated Mach-O dynamic-symbol-table command";
                return false;
            }
            indirectSymbolsOffset = U32(bytes, cursor + 56);
            indirectSymbolCount = U32(bytes, cursor + 60);
        }
        else if (command == kCodeSignature) {
            if (size < 16) {
                error = "truncated Mach-O code-signature command";
                return false;
            }
            image.codeSignature = MachORange{U32(bytes, cursor + 8), U32(bytes, cursor + 12)};
        }
        cursor += size;
    }
    if (cursor != commandEnd) {
        error = "Mach-O load-command count and size disagree";
        return false;
    }

    if (symbolOffset && stringOffset) {
        const std::size_t symbolsSize = static_cast<std::size_t>(symbolCount) * 16;
        if (!InBounds(bytes, *symbolOffset, symbolsSize) || !InBounds(bytes, *stringOffset, stringSize)) {
            error = "Mach-O symbol or string table extends beyond the image";
            return false;
        }
        for (std::uint32_t index = 0; index < symbolCount; ++index) {
            const std::size_t offset = *symbolOffset + static_cast<std::size_t>(index) * 16;
            const std::uint32_t nameOffset = U32(bytes, offset);
            if (nameOffset >= stringSize) {
                error = "Mach-O symbol name is outside the string table";
                return false;
            }
            const auto name = CommandString(bytes, *stringOffset, stringSize, nameOffset);
            if (!name) {
                error = "unterminated Mach-O symbol name";
                return false;
            }
            image.symbols.push_back(
                {*name, bytes[offset + 4], bytes[offset + 5], U16(bytes, offset + 6), U64(bytes, offset + 8)});
        }
    }
    if (indirectSymbolsOffset) {
        const std::size_t tableSize = static_cast<std::size_t>(indirectSymbolCount) * 4;
        if (!InBounds(bytes, *indirectSymbolsOffset, tableSize)) {
            error = "Mach-O indirect-symbol table extends beyond the image";
            return false;
        }
        for (std::uint32_t index = 0; index < indirectSymbolCount; ++index) {
            image.indirectSymbols.push_back(U32(bytes, *indirectSymbolsOffset + static_cast<std::size_t>(index) * 4));
        }
    }
    if (image.dyldInfo && image.dyldInfo->bindSize != 0) {
        if (!InBounds(bytes, image.dyldInfo->bindOffset, image.dyldInfo->bindSize)) {
            error = "Mach-O bind stream extends beyond the image";
            return false;
        }
        std::size_t bindCursor = image.dyldInfo->bindOffset;
        const std::size_t bindEnd = bindCursor + image.dyldInfo->bindSize;
        std::uint8_t segmentIndex = 0;
        std::uint64_t segmentOffset = 0;
        std::uint32_t libraryOrdinal = 0;
        std::string symbol;
        bool done = false;
        while (bindCursor < bindEnd && !done) {
            const std::uint8_t byte = bytes[bindCursor++];
            const std::uint8_t opcode = byte & 0xF0;
            const std::uint8_t immediate = byte & 0x0F;
            switch (opcode) {
            case 0x00: // BIND_OPCODE_DONE
                done = true;
                break;
            case 0x10: // BIND_OPCODE_SET_DYLIB_ORDINAL_IMM
                libraryOrdinal = immediate;
                break;
            case 0x20: { // BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB
                std::uint64_t ordinal = 0;
                if (!ReadUleb128(bytes, bindCursor, bindEnd, ordinal) ||
                    ordinal > std::numeric_limits<std::uint32_t>::max()) {
                    error = "invalid Mach-O bind library ordinal";
                    return false;
                }
                libraryOrdinal = static_cast<std::uint32_t>(ordinal);
                break;
            }
            case 0x40: { // BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM
                const std::size_t begin = bindCursor;
                while (bindCursor < bindEnd && bytes[bindCursor] != 0) {
                    ++bindCursor;
                }
                if (bindCursor == bindEnd) {
                    error = "unterminated Mach-O bind symbol";
                    return false;
                }
                symbol.assign(reinterpret_cast<const char *>(bytes.data() + begin), bindCursor - begin);
                ++bindCursor;
                break;
            }
            case 0x50: // BIND_OPCODE_SET_TYPE_IMM
                break;
            case 0x70: { // BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB
                segmentIndex = immediate;
                if (!ReadUleb128(bytes, bindCursor, bindEnd, segmentOffset)) {
                    error = "invalid Mach-O bind segment offset";
                    return false;
                }
                break;
            }
            case 0x90: // BIND_OPCODE_DO_BIND
                if (symbol.empty()) {
                    error = "Mach-O bind action has no symbol";
                    return false;
                }
                image.binds.push_back({segmentIndex, segmentOffset, libraryOrdinal, symbol});
                if (segmentOffset > std::numeric_limits<std::uint64_t>::max() - 8) {
                    error = "Mach-O bind segment offset overflows";
                    return false;
                }
                segmentOffset += 8;
                break;
            default:
                error = "unsupported Mach-O bind opcode in portable reader";
                return false;
            }
        }
        if (!done) {
            error = "unterminated Mach-O bind stream";
            return false;
        }
    }
    if (image.dyldInfo && image.dyldInfo->rebaseSize != 0) {
        if (!InBounds(bytes, image.dyldInfo->rebaseOffset, image.dyldInfo->rebaseSize)) {
            error = "Mach-O rebase stream extends beyond the image";
            return false;
        }
        std::size_t rebaseCursor = image.dyldInfo->rebaseOffset;
        const std::size_t rebaseEnd = rebaseCursor + image.dyldInfo->rebaseSize;
        std::uint8_t segmentIndex = 0;
        std::uint64_t segmentOffset = 0;
        bool pointerType = false;
        bool done = false;
        while (rebaseCursor < rebaseEnd && !done) {
            const std::uint8_t byte = bytes[rebaseCursor++];
            const std::uint8_t opcode = byte & 0xF0;
            const std::uint8_t immediate = byte & 0x0F;
            switch (opcode) {
            case 0x00: // REBASE_OPCODE_DONE
                done = true;
                break;
            case 0x10: // REBASE_OPCODE_SET_TYPE_IMM
                if (immediate != 1) {
                    error = "unsupported Mach-O rebase type in portable reader";
                    return false;
                }
                pointerType = true;
                break;
            case 0x20: // REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB
                segmentIndex = immediate;
                if (!ReadUleb128(bytes, rebaseCursor, rebaseEnd, segmentOffset)) {
                    error = "invalid Mach-O rebase segment offset";
                    return false;
                }
                break;
            case 0x50: // REBASE_OPCODE_DO_REBASE_IMM_TIMES
                if (!pointerType || immediate == 0) {
                    error = "invalid Mach-O rebase action";
                    return false;
                }
                for (std::uint8_t count = 0; count < immediate; ++count) {
                    image.rebases.push_back({segmentIndex, segmentOffset});
                    if (segmentOffset > std::numeric_limits<std::uint64_t>::max() - 8) {
                        error = "Mach-O rebase segment offset overflows";
                        return false;
                    }
                    segmentOffset += 8;
                }
                break;
            default:
                error = "unsupported Mach-O rebase opcode in portable reader";
                return false;
            }
        }
        if (!done) {
            error = "unterminated Mach-O rebase stream";
            return false;
        }
    }
    if (image.codeSignature && !InBounds(bytes, image.codeSignature->offset, image.codeSignature->size)) {
        error = "Mach-O code signature extends beyond the image";
        return false;
    }
    if (image.codeSignature && image.codeSignature->size >= 20 &&
        BigU32(bytes, image.codeSignature->offset) == 0xFADE'0CC0) {
        const std::size_t signatureOffset = image.codeSignature->offset;
        const std::size_t signatureEnd = signatureOffset + image.codeSignature->size;
        const std::uint32_t blobLength = BigU32(bytes, signatureOffset + 4);
        const std::uint32_t blobCount = BigU32(bytes, signatureOffset + 8);
        if (blobLength != image.codeSignature->size || blobCount == 0 ||
            blobCount > (image.codeSignature->size - 12) / 8) {
            error = "invalid Mach-O embedded-signature superblob";
            return false;
        }
        std::optional<std::size_t> directoryOffset;
        for (std::uint32_t index = 0; index < blobCount; ++index) {
            const std::size_t entry = signatureOffset + 12 + static_cast<std::size_t>(index) * 8;
            if (BigU32(bytes, entry) == 0) {
                directoryOffset = signatureOffset + BigU32(bytes, entry + 4);
                break;
            }
        }
        if (!directoryOffset || *directoryOffset < signatureOffset || *directoryOffset > signatureEnd ||
            signatureEnd - *directoryOffset < 88 || BigU32(bytes, *directoryOffset) != 0xFADE'0C02) {
            error = "Mach-O signature has no valid CodeDirectory";
            return false;
        }
        const std::uint32_t directoryLength = BigU32(bytes, *directoryOffset + 4);
        const std::uint32_t hashOffset = BigU32(bytes, *directoryOffset + 16);
        const std::uint32_t identifierOffset = BigU32(bytes, *directoryOffset + 20);
        const std::uint32_t specialSlots = BigU32(bytes, *directoryOffset + 24);
        const std::uint32_t codeSlots = BigU32(bytes, *directoryOffset + 28);
        const std::uint8_t hashSize = bytes[*directoryOffset + 36];
        if (directoryLength > signatureEnd - *directoryOffset || identifierOffset >= directoryLength ||
            hashOffset > directoryLength || specialSlots != 0 || hashSize == 0 ||
            codeSlots > (directoryLength - hashOffset) / hashSize) {
            error = "invalid Mach-O CodeDirectory offsets or slot counts";
            return false;
        }
        std::size_t identifierEnd = *directoryOffset + identifierOffset;
        const std::size_t hashBegin = *directoryOffset + hashOffset;
        while (identifierEnd < hashBegin && bytes[identifierEnd] != 0) {
            ++identifierEnd;
        }
        if (identifierEnd == hashBegin) {
            error = "unterminated Mach-O CodeDirectory identifier";
            return false;
        }
        MachOCodeDirectory directory;
        directory.version = BigU32(bytes, *directoryOffset + 8);
        directory.flags = BigU32(bytes, *directoryOffset + 12);
        directory.codeLimit = BigU32(bytes, *directoryOffset + 32);
        directory.hashSize = hashSize;
        directory.hashType = bytes[*directoryOffset + 37];
        directory.pageSizePower = bytes[*directoryOffset + 39];
        directory.executableSegmentBase = BigU64(bytes, *directoryOffset + 64);
        directory.executableSegmentLimit = BigU64(bytes, *directoryOffset + 72);
        directory.executableSegmentFlags = BigU64(bytes, *directoryOffset + 80);
        directory.identifier =
            std::string(reinterpret_cast<const char *>(bytes.data() + *directoryOffset + identifierOffset),
                        identifierEnd - (*directoryOffset + identifierOffset));
        for (std::uint32_t slot = 0; slot < codeSlots; ++slot) {
            const std::size_t slotOffset = hashBegin + static_cast<std::size_t>(slot) * hashSize;
            directory.codeHashes.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(slotOffset),
                                              bytes.begin() + static_cast<std::ptrdiff_t>(slotOffset + hashSize));
        }
        image.codeDirectory = std::move(directory);
    }
    return true;
}
} // namespace Rux::Testing
