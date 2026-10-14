// Decoding of the on-disk RCU object format `RcuWriter.cpp` produces.

#include "Object/Rcu/RcuReader.h"

#include "Object/Rcu/RcuSerialization.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace Rux {
namespace {
constexpr std::size_t kHeaderSize = 32;
constexpr std::size_t kSectionEntrySize = 40;
constexpr std::size_t kSymbolEntrySize = 20;
constexpr std::size_t kRelocationSize = 16;
constexpr std::size_t kMetadataSize = 64;
constexpr std::size_t kChecksumOffset = 28;

/// Reads little-endian fields at offsets that are checked against the end before every read.
class ByteReader {
public:
    explicit ByteReader(const std::span<const std::uint8_t> inputBytes)
        : bytes(inputBytes) {
    }

    [[nodiscard]] bool Has(const std::size_t offset, const std::size_t length) const noexcept {
        return offset <= bytes.size() && length <= bytes.size() - offset;
    }

    [[nodiscard]] std::uint64_t Read(const std::size_t offset, const std::size_t width) const noexcept {
        std::uint64_t value = 0;
        for (std::size_t index = 0; index < width; ++index)
            value |= static_cast<std::uint64_t>(bytes[offset + index]) << (8 * index);
        return value;
    }

    [[nodiscard]] std::uint8_t U8(const std::size_t offset) const noexcept {
        return bytes[offset];
    }

    [[nodiscard]] std::uint16_t U16(const std::size_t offset) const noexcept {
        return static_cast<std::uint16_t>(Read(offset, 2));
    }

    [[nodiscard]] std::uint32_t U32(const std::size_t offset) const noexcept {
        return static_cast<std::uint32_t>(Read(offset, 4));
    }

    [[nodiscard]] std::uint64_t U64(const std::size_t offset) const noexcept {
        return Read(offset, 8);
    }

    [[nodiscard]] std::span<const std::uint8_t> Bytes(const std::size_t offset, const std::size_t length) const {
        return bytes.subspan(offset, length);
    }

private:
    std::span<const std::uint8_t> bytes;
};

/// The NUL-terminated string at `offset` of the string table, or nullopt when it runs past the table.
std::optional<std::string> TableString(const ByteReader &reader, const std::size_t tableOffset,
                                       const std::size_t tableSize, const std::uint32_t offset) {
    if (offset == 0)
        return std::string{};
    if (offset >= tableSize)
        return std::nullopt;
    const auto table = reader.Bytes(tableOffset, tableSize);
    const auto *begin = table.data() + offset;
    const auto *end = static_cast<const std::uint8_t *>(std::memchr(begin, 0, tableSize - offset));
    if (end == nullptr)
        return std::nullopt;
    return std::string(reinterpret_cast<const char *>(begin), static_cast<std::size_t>(end - begin));
}
} // namespace

std::optional<RcuFile> DeserializeRcuFile(const std::span<const std::uint8_t> bytes) {
    const ByteReader reader(bytes);
    if (!reader.Has(0, kHeaderSize) || reader.U8(0) != 0x52 || reader.U8(1) != 0x43 || reader.U8(2) != 0x55 ||
        reader.U8(3) != 0x00 || reader.U16(4) != 0x0100)
        return std::nullopt;
    std::vector<std::uint8_t> unsummed(bytes.begin(), bytes.end());
    std::memset(unsummed.data() + kChecksumOffset, 0, 4);
    if (RcuChecksum(unsummed) != reader.U32(kChecksumOffset))
        return std::nullopt;

    RcuFile file;
    file.arch = reader.U8(6);
    file.flags = reader.U8(7);
    const std::size_t sectionCount = reader.U16(8);
    const std::size_t symbolCount = reader.U32(12);
    const std::size_t tableOffset = reader.U32(16);
    const std::size_t tableSize = reader.U32(20);
    const std::size_t metadataOffset = reader.U32(24);
    if (!reader.Has(kHeaderSize, sectionCount * kSectionEntrySize) ||
        !reader.Has(kHeaderSize + sectionCount * kSectionEntrySize, symbolCount * kSymbolEntrySize) ||
        !reader.Has(tableOffset, tableSize))
        return std::nullopt;
    const auto string = [&](const std::uint32_t offset) { return TableString(reader, tableOffset, tableSize, offset); };

    file.sections.reserve(sectionCount);
    for (std::size_t index = 0; index < sectionCount; ++index) {
        const std::size_t entry = kHeaderSize + index * kSectionEntrySize;
        RcuSection section;
        const auto name = reader.Bytes(entry, 8);
        section.name.assign(name.begin(), std::ranges::find(name, std::uint8_t{0}));
        section.type = reader.U32(entry + 8);
        section.flags = reader.U32(entry + 12);
        const std::size_t rawOffset = reader.U32(entry + 16);
        const std::size_t rawSize = reader.U32(entry + 20);
        section.alignment = reader.U16(entry + 28);
        const std::size_t relocationCount = reader.U16(entry + 30);
        const std::size_t relocationOffset = reader.U32(entry + 32);
        if (rawSize != 0) {
            if (!reader.Has(rawOffset, rawSize))
                return std::nullopt;
            const auto data = reader.Bytes(rawOffset, rawSize);
            section.data.assign(data.begin(), data.end());
        }
        if (relocationCount != 0 && !reader.Has(relocationOffset, relocationCount * kRelocationSize))
            return std::nullopt;
        section.relocs.reserve(relocationCount);
        for (std::size_t relocation = 0; relocation < relocationCount; ++relocation) {
            const std::size_t at = relocationOffset + relocation * kRelocationSize;
            section.relocs.push_back({reader.U32(at), reader.U32(at + 4), reader.U16(at + 8),
                                      static_cast<std::int32_t>(reader.U32(at + 12))});
        }
        file.sections.push_back(std::move(section));
    }

    file.symbols.reserve(symbolCount);
    const std::size_t symbolTable = kHeaderSize + sectionCount * kSectionEntrySize;
    for (std::size_t index = 0; index < symbolCount; ++index) {
        const std::size_t entry = symbolTable + index * kSymbolEntrySize;
        RcuSymbol symbol;
        auto name = string(reader.U32(entry));
        auto typeName = string(reader.U32(entry + 16));
        if (!name || !typeName)
            return std::nullopt;
        symbol.name = std::move(*name);
        symbol.typeName = std::move(*typeName);
        symbol.value = reader.U32(entry + 4);
        symbol.size = reader.U32(entry + 8);
        symbol.sectionIdx = reader.U16(entry + 12);
        symbol.kind = reader.U8(entry + 14);
        symbol.visibility = reader.U8(entry + 15);
        file.symbols.push_back(std::move(symbol));
    }

    if (metadataOffset != 0) {
        if (!reader.Has(metadataOffset, kMetadataSize) || reader.U8(metadataOffset) != 0x4D ||
            reader.U8(metadataOffset + 1) != 0x45 || reader.U8(metadataOffset + 2) != 0x54 ||
            reader.U8(metadataOffset + 3) != 0x41)
            return std::nullopt;
        auto sourcePath = string(reader.U32(metadataOffset + 8));
        auto packageName = string(reader.U32(metadataOffset + 12));
        if (!sourcePath || !packageName)
            return std::nullopt;
        file.hasMetadata = true;
        file.sourcePath = std::move(*sourcePath);
        file.packageName = std::move(*packageName);
        file.buildTimestamp = reader.U64(metadataOffset + 16);
        file.ruxVersion = reader.U32(metadataOffset + 24);
        file.compilerFlags = reader.U32(metadataOffset + 28);
        const auto hash = reader.Bytes(metadataOffset + 32, file.sourceHash.size());
        std::ranges::copy(hash, file.sourceHash.begin());
    }
    return file;
}

std::optional<RcuFile> RcuReader::Read(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
        return std::nullopt;
    const auto end = stream.tellg();
    if (end < 0)
        return std::nullopt;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        return std::nullopt;
    return DeserializeRcuFile(bytes);
}
} // namespace Rux
