#pragma once

// Test-owned ELF64 reader used instead of host tools or host ELF headers.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <vector>

namespace Rux::Testing {
struct ElfImage {
    struct Segment {
        std::uint32_t type;
        std::uint32_t flags;
        std::uint64_t offset;
        std::uint64_t address;
        std::uint64_t fileSize;
        std::uint64_t memorySize;
        std::uint64_t alignment;
    };

    struct DynamicSymbol {
        std::string name;
        std::uint8_t info;
        std::uint16_t sectionIndex;
        std::uint64_t value;
        std::uint64_t size;
    };

    struct Rela {
        std::uint64_t offset;
        std::uint32_t symbolIndex;
        std::uint32_t type;
        std::int64_t addend;
    };

    std::vector<std::uint8_t> bytes;

    [[nodiscard]] std::uint8_t OsAbi() const {
        return bytes[7];
    }

    [[nodiscard]] std::uint16_t Read16(const std::size_t offset) const {
        return static_cast<std::uint16_t>(bytes[offset] | bytes[offset + 1] << 8U);
    }

    [[nodiscard]] std::uint32_t Read32(const std::size_t offset) const {
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            value |= static_cast<std::uint32_t>(bytes[offset + i]) << (i * 8U);
        }
        return value;
    }

    [[nodiscard]] std::uint64_t Read64(const std::size_t offset) const {
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            value |= static_cast<std::uint64_t>(bytes[offset + i]) << (i * 8U);
        }
        return value;
    }

    [[nodiscard]] std::uint16_t Type() const {
        return Read16(16);
    }

    [[nodiscard]] std::uint16_t Machine() const {
        return Read16(18);
    }

    [[nodiscard]] std::uint64_t Entry() const {
        return Read64(24);
    }

    [[nodiscard]] std::vector<Segment> Segments() const {
        const auto table = static_cast<std::size_t>(Read64(32));
        const auto entrySize = static_cast<std::size_t>(Read16(54));
        std::vector<Segment> segments;
        for (std::size_t i = 0; i < Read16(56); ++i) {
            const std::size_t header = table + i * entrySize;
            segments.push_back({Read32(header), Read32(header + 4), Read64(header + 8), Read64(header + 16),
                                Read64(header + 32), Read64(header + 40), Read64(header + 48)});
        }
        return segments;
    }

    [[nodiscard]] std::optional<Segment> SegmentOfType(const std::uint32_t type) const {
        const auto segments = Segments();
        const auto found = std::ranges::find(segments, type, &Segment::type);
        return found == segments.end() ? std::nullopt : std::optional(*found);
    }

    [[nodiscard]] std::size_t OffsetOf(const std::uint64_t address) const {
        for (const auto &segment : Segments()) {
            if (segment.type == 1 && address >= segment.address && address < segment.address + segment.fileSize) {
                return static_cast<std::size_t>(segment.offset + address - segment.address);
            }
        }
        return 0;
    }

    [[nodiscard]] std::span<const std::uint8_t> MappedBytes(const std::uint64_t address, const std::size_t size) const {
        for (const auto &segment : Segments()) {
            if (segment.type != 1 || address < segment.address) {
                continue;
            }
            const std::uint64_t delta = address - segment.address;
            if (delta <= segment.fileSize && size <= segment.fileSize - delta) {
                return std::span<const std::uint8_t>(bytes).subspan(segment.offset + delta, size);
            }
        }
        return {};
    }

    [[nodiscard]] std::uint64_t WritableSegmentAddress() const {
        for (const auto &segment : Segments()) {
            if (segment.type == 1 && (segment.flags & 0x2U) != 0) {
                return segment.address;
            }
        }
        return 0;
    }

    [[nodiscard]] std::uint32_t Word(const std::uint64_t address) const {
        return Read32(OffsetOf(address));
    }

    [[nodiscard]] std::uint64_t Giant(const std::uint64_t address) const {
        return Read64(OffsetOf(address));
    }

    [[nodiscard]] std::uint64_t LoadAlignment() const {
        std::uint64_t alignment = 0;
        for (const auto &segment : Segments()) {
            if (segment.type == 1) {
                if (alignment != 0 && alignment != segment.alignment) {
                    return 0;
                }
                alignment = segment.alignment;
            }
        }
        return alignment;
    }

    [[nodiscard]] std::uint64_t DynamicTag(const std::uint64_t tag) const {
        const auto dynamic = SegmentOfType(2); // PT_DYNAMIC
        if (!dynamic) {
            return 0;
        }
        for (std::size_t at = static_cast<std::size_t>(dynamic->offset); at + 16 <= dynamic->offset + dynamic->fileSize;
             at += 16) {
            if (Read64(at) == tag) {
                return Read64(at + 8);
            }
        }
        return 0;
    }

    [[nodiscard]] std::string Interpreter() const {
        const auto interp = SegmentOfType(3); // PT_INTERP
        return interp ? StringAtOffset(interp->offset) : std::string{};
    }

    [[nodiscard]] std::vector<std::string> NeededLibraries() const {
        const auto dynamic = SegmentOfType(2); // PT_DYNAMIC
        std::vector<std::string> libraries;
        if (!dynamic) {
            return libraries;
        }
        const std::uint64_t strings = DynamicTag(5); // DT_STRTAB
        for (std::size_t at = static_cast<std::size_t>(dynamic->offset); at + 16 <= dynamic->offset + dynamic->fileSize;
             at += 16) {
            if (Read64(at) == 1) { // DT_NEEDED
                libraries.push_back(StringAtAddress(strings + Read64(at + 8)));
            }
        }
        return libraries;
    }

    [[nodiscard]] std::string Soname() const {
        const std::uint64_t strings = DynamicTag(5);                                     // DT_STRTAB
        return strings == 0 ? std::string{} : StringAtAddress(strings + DynamicTag(14)); // DT_SONAME
    }

    [[nodiscard]] std::vector<DynamicSymbol> DynamicSymbols() const {
        const std::uint64_t table = DynamicTag(6);   // DT_SYMTAB
        const std::uint64_t strings = DynamicTag(5); // DT_STRTAB
        const std::uint64_t hash = DynamicTag(4);    // DT_HASH
        if (table == 0 || strings == 0 || hash == 0) {
            return {};
        }
        const std::uint32_t count = Read32(OffsetOf(hash) + 4); // nchain
        std::vector<DynamicSymbol> symbols;
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::size_t offset = OffsetOf(table + static_cast<std::uint64_t>(i) * 24);
            symbols.push_back({StringAtAddress(strings + Read32(offset)), bytes[offset + 4], Read16(offset + 6),
                               Read64(offset + 8), Read64(offset + 16)});
        }
        return symbols;
    }

    [[nodiscard]] std::optional<std::uint32_t> HashedDynamicSymbolIndex(const std::string &name) const {
        const std::uint64_t hashAddress = DynamicTag(4); // DT_HASH
        if (hashAddress == 0) {
            return std::nullopt;
        }
        const std::size_t hash = OffsetOf(hashAddress);
        const std::uint32_t bucketCount = Read32(hash);
        const std::uint32_t symbolCount = Read32(hash + 4);
        if (bucketCount == 0) {
            return std::nullopt;
        }
        const auto symbols = DynamicSymbols();
        std::uint32_t index = Read32(hash + 8 + ElfHash(name) % bucketCount * 4);
        for (std::uint32_t traversed = 0;
             index != 0 && index < symbolCount && index < symbols.size() && traversed < symbolCount; ++traversed) {
            if (symbols[index].name == name) {
                return index;
            }
            index = Read32(hash + 8 + static_cast<std::size_t>(bucketCount + index) * 4);
        }
        return std::nullopt;
    }

    [[nodiscard]] std::vector<Rela> Relocations(const std::uint64_t address, const std::uint64_t size) const {
        std::vector<Rela> relocations;
        for (std::uint64_t at = 0; at < size; at += 24) {
            const std::size_t offset = OffsetOf(address + at);
            const std::uint64_t info = Read64(offset + 8);
            relocations.push_back({Read64(offset), static_cast<std::uint32_t>(info >> 32U),
                                   static_cast<std::uint32_t>(info), static_cast<std::int64_t>(Read64(offset + 16))});
        }
        return relocations;
    }

    [[nodiscard]] std::vector<Rela> DynamicRelocations() const {
        return Relocations(DynamicTag(7), DynamicTag(8)); // DT_RELA / DT_RELASZ
    }

    [[nodiscard]] std::vector<Rela> PltRelocations() const {
        return Relocations(DynamicTag(23), DynamicTag(2)); // DT_JMPREL / DT_PLTRELSZ
    }

    [[nodiscard]] std::uint64_t GotSlotReachedBy(const std::uint64_t address) const {
        const std::uint32_t adrp = Word(address);
        const auto immediate =
            static_cast<std::int32_t>(((adrp >> 5U & 0x7FFFFU) << 2U | (adrp >> 29U & 3U)) << 11U) >> 11;
        const std::uint64_t page = (address & ~std::uint64_t{0xFFF}) + (static_cast<std::int64_t>(immediate) << 12U);
        const std::uint64_t viaLoad = page + ((Word(address + 4) >> 10U & 0xFFFU) << 3U);
        const std::uint64_t viaAdd = page + (Word(address + 8) >> 10U & 0xFFFU);
        return viaLoad == viaAdd ? viaLoad : 0;
    }

private:
    [[nodiscard]] static std::uint32_t ElfHash(const std::string &name) {
        std::uint32_t hash = 0;
        for (const unsigned char character : name) {
            hash = (hash << 4U) + character;
            const std::uint32_t high = hash & 0xF0000000U;
            if (high != 0) {
                hash ^= high >> 24U;
            }
            hash &= ~high;
        }
        return hash;
    }

    [[nodiscard]] std::string StringAtAddress(const std::uint64_t address) const {
        return StringAtOffset(OffsetOf(address));
    }

    [[nodiscard]] std::string StringAtOffset(const std::size_t offset) const {
        const auto begin = bytes.begin() + static_cast<std::ptrdiff_t>(offset);
        return {begin, std::find(begin, bytes.end(), 0)};
    }
};
} // namespace Rux::Testing
