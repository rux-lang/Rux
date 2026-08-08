// Builds the .ruxpkg archive that publication uploads: a Stored-only ZIP whose
// root Rux.toml matches the manifest bytes sent beside it.

#include "Package/Artifact.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <ranges>
#include <string_view>
#include <system_error>
#include <vector>

namespace Rux {
namespace fs = std::filesystem;
using ArtifactResult = std::expected<PackageArtifact, std::string>;

namespace {
/// One regular file destined for the archive.
struct ArtifactEntry {
    std::string path; ///< Logical, `/`-separated archive path.
    std::string data;
};

/**
 * @brief The IEEE CRC-32 that the ZIP format requires.
 *
 * Object/Rcu uses the Castagnoli polynomial for its own container checksums, so
 * the two are not interchangeable and this table stays local.
 */
std::uint32_t Crc32(const std::string_view data) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> entries{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1U) != 0 ? 0xEDB88320U ^ (value >> 1U) : value >> 1U;
            }
            entries[i] = value;
        }
        return entries;
    }();

    std::uint32_t crc = 0xFFFFFFFFU;
    for (const unsigned char byte : data) {
        crc = table[(crc ^ byte) & 0xFFU] ^ (crc >> 8U);
    }
    return crc ^ 0xFFFFFFFFU;
}

/**
 * @brief Whether the text is well-formed UTF-8.
 *
 * The registry rejects a manifest, source or referenced text file that is not,
 * so catching it here turns a remote diagnostic into a local one.
 */
bool IsUtf8(const std::string_view text) {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto lead = static_cast<unsigned char>(text[index]);
        std::size_t length = 0;
        std::uint32_t codePoint = 0;
        if (lead < 0x80) {
            length = 1;
            codePoint = lead;
        }
        else if ((lead & 0xE0U) == 0xC0U) {
            length = 2;
            codePoint = lead & 0x1FU;
        }
        else if ((lead & 0xF0U) == 0xE0U) {
            length = 3;
            codePoint = lead & 0x0FU;
        }
        else if ((lead & 0xF8U) == 0xF0U) {
            length = 4;
            codePoint = lead & 0x07U;
        }
        else {
            return false;
        }

        if (index + length > text.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto continuation = static_cast<unsigned char>(text[index + offset]);
            if ((continuation & 0xC0U) != 0x80U) {
                return false;
            }
            codePoint = (codePoint << 6U) | (continuation & 0x3FU);
        }

        // Reject overlong forms, surrogates and values above the Unicode range.
        static constexpr std::array<std::uint32_t, 5> minimum{0, 0x0, 0x80, 0x800, 0x10000};
        if (codePoint < minimum[length] || codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF)) {
            return false;
        }
        index += length;
    }
    return true;
}

/// Whether the component names an MS-DOS device, which some platforms cannot create.
bool IsReservedDeviceName(std::string_view component) {
    if (const auto dot = component.find('.'); dot != std::string_view::npos) {
        component = component.substr(0, dot);
    }
    std::string upper(component);
    std::ranges::transform(upper, upper.begin(),
                           [](const unsigned char c) { return static_cast<char>(std::toupper(c)); });

    static constexpr std::array<std::string_view, 4> devices{"CON", "PRN", "AUX", "NUL"};
    if (std::ranges::contains(devices, upper)) {
        return true;
    }
    static constexpr std::array<std::string_view, 2> numbered{"COM", "LPT"};
    return upper.size() == 4 && std::ranges::contains(numbered, std::string_view(upper).substr(0, 3)) &&
           upper[3] >= '1' && upper[3] <= '9';
}

/**
 * @brief Check a logical archive path against the portable-path rules.
 *
 * These are the rules that let one archive extract to the same tree on
 * case-sensitive and case-insensitive platforms alike.
 *
 * @return An empty string when the path is portable, otherwise the reason
 */
std::string RejectEntryPath(const std::string_view path) {
    if (path.empty()) {
        return "the entry path is empty";
    }
    if (path.size() > artifactMaxEntryPathBytes) {
        return std::format("the entry path exceeds {} bytes", artifactMaxEntryPathBytes);
    }
    if (!IsUtf8(path)) {
        return "the entry path is not valid UTF-8";
    }
    if (path.contains('\\')) {
        return "the entry path contains a backslash";
    }
    if (path.starts_with('/')) {
        return "the entry path is absolute";
    }
    if (path.size() >= 2 && path[1] == ':') {
        return "the entry path is drive-qualified";
    }
    for (const unsigned char c : path) {
        if (c < 0x20 || c == 0x7F) {
            return "the entry path contains a control character";
        }
        if (std::string_view("<>:\"|?*").contains(static_cast<char>(c))) {
            return std::format("the entry path contains the reserved character '{}'", static_cast<char>(c));
        }
    }

    for (const auto component : std::views::split(path, '/')) {
        const std::string_view text(component.begin(), component.end());
        if (text.empty()) {
            return "the entry path has an empty component";
        }
        if (text == "." || text == "..") {
            return std::format("the entry path has a '{}' component", text);
        }
        if (text.ends_with('.') || text.ends_with(' ')) {
            return std::format("the entry path component '{}' ends with a dot or space", text);
        }
        if (IsReservedDeviceName(text)) {
            return std::format("the entry path component '{}' is a reserved device name", text);
        }
    }
    return {};
}

/// Read a whole file in binary mode, preserving its exact bytes.
std::expected<std::string, std::string> ReadFileBytes(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::unexpected(std::format("failed to read '{}'", path.generic_string()));
    }
    std::string data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (input.bad()) {
        return std::unexpected(std::format("failed to read '{}'", path.generic_string()));
    }
    return data;
}

bool IsRuxSource(const std::string_view entryPath) {
    return entryPath.starts_with("Src/") && entryPath.ends_with(".rux");
}

/**
 * @brief Collect every regular file below the package's Src/ directory.
 *
 * Symlinks and other special entries are rejected rather than followed, so the
 * archive can never carry a link out of the package.
 */
std::expected<void, std::string> CollectSources(const fs::path &root, std::vector<ArtifactEntry> &entries) {
    const fs::path sourceRoot = root / "Src";
    std::error_code ec;
    if (!fs::is_directory(sourceRoot, ec)) {
        return std::unexpected("the package has no Src directory");
    }

    fs::recursive_directory_iterator walk(sourceRoot, fs::directory_options::none, ec);
    if (ec) {
        return std::unexpected(std::format("failed to read '{}'", sourceRoot.generic_string()));
    }
    for (const auto &item : walk) {
        if (item.is_directory(ec)) {
            continue;
        }
        const std::string entryPath = ("Src" / fs::relative(item.path(), sourceRoot)).generic_string();
        if (item.is_symlink(ec) || !item.is_regular_file(ec)) {
            return std::unexpected(std::format("'{}' is not a regular file", entryPath));
        }
        auto data = ReadFileBytes(item.path());
        if (!data) {
            return std::unexpected(std::move(data.error()));
        }
        entries.emplace_back(entryPath, std::move(*data));
    }
    return {};
}

/**
 * @brief Append one Stored entry to the archive and record its central header.
 */
void WriteEntry(const ArtifactEntry &entry, std::string &archive, std::string &directory) {
    // 1980-01-01 00:00:00, the earliest representable MS-DOS timestamp. A fixed
    // value keeps `rux pack` byte-for-byte reproducible.
    constexpr std::uint16_t dosTime = 0;
    constexpr std::uint16_t dosDate = 0x0021;
    constexpr std::uint16_t utf8NameFlag = 0x0800;
    constexpr std::uint16_t storedMethod = 0;
    constexpr std::uint16_t versionNeeded = 20;

    const auto put16 = [](std::string &out, const std::uint16_t value) {
        out.push_back(static_cast<char>(value & 0xFFU));
        out.push_back(static_cast<char>((value >> 8U) & 0xFFU));
    };
    const auto put32 = [](std::string &out, const std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            out.push_back(static_cast<char>((value >> shift) & 0xFFU));
        }
    };

    const auto offset = static_cast<std::uint32_t>(archive.size());
    const std::uint32_t crc = Crc32(entry.data);
    const auto size = static_cast<std::uint32_t>(entry.data.size());
    const auto nameLength = static_cast<std::uint16_t>(entry.path.size());

    put32(archive, 0x04034B50); // Local file header signature
    put16(archive, versionNeeded);
    put16(archive, utf8NameFlag);
    put16(archive, storedMethod);
    put16(archive, dosTime);
    put16(archive, dosDate);
    put32(archive, crc);
    put32(archive, size);
    put32(archive, size);
    put16(archive, nameLength);
    put16(archive, 0); // Extra field length
    archive.append(entry.path);
    archive.append(entry.data);

    put32(directory, 0x02014B50); // Central directory header signature
    put16(directory, versionNeeded);
    put16(directory, versionNeeded);
    put16(directory, utf8NameFlag);
    put16(directory, storedMethod);
    put16(directory, dosTime);
    put16(directory, dosDate);
    put32(directory, crc);
    put32(directory, size);
    put32(directory, size);
    put16(directory, nameLength);
    put16(directory, 0); // Extra field length
    put16(directory, 0); // File comment length
    put16(directory, 0); // Disk number start
    put16(directory, 0); // Internal file attributes
    put32(directory, 0); // External file attributes: a regular MS-DOS file
    put32(directory, offset);
    directory.append(entry.path);
}

/// Read a little-endian field, or nullopt when it runs past the end.
std::optional<std::uint32_t> Read32(const std::string_view archive, const std::size_t offset) {
    if (offset + 4 > archive.size()) {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(archive[offset + i])) << (8 * i);
    }
    return value;
}

std::optional<std::uint16_t> Read16(const std::string_view archive, const std::size_t offset) {
    if (offset + 2 > archive.size()) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(static_cast<unsigned char>(archive[offset]) |
                                      static_cast<unsigned>(static_cast<unsigned char>(archive[offset + 1])) << 8);
}

/// One entry recovered from an archive, before it reaches the filesystem.
struct ExtractedEntry {
    std::string path;
    std::string_view data;
};

/**
 * @brief Locate the end-of-central-directory record.
 *
 * `rux pack` writes no archive comment, so the record is normally the last 22
 * bytes; the scan tolerates one anyway because a third-party packer may add it.
 */
std::optional<std::size_t> FindEndOfCentralDirectory(const std::string_view archive) {
    constexpr std::size_t recordSize = 22;
    constexpr std::size_t maxComment = 0xFFFF;
    if (archive.size() < recordSize) {
        return std::nullopt;
    }
    const std::size_t lowest = archive.size() - recordSize >= maxComment ? archive.size() - recordSize - maxComment : 0;
    for (std::size_t offset = archive.size() - recordSize + 1; offset-- > lowest;) {
        if (Read32(archive, offset) == 0x06054B50U) {
            const auto commentLength = Read16(archive, offset + 20);
            if (commentLength && offset + recordSize + *commentLength == archive.size()) {
                return offset;
            }
        }
    }
    return std::nullopt;
}

/**
 * @brief Read every central-directory record and slice out its file data.
 *
 * The central directory is authoritative: each record names the local header it
 * belongs to, and the local header only supplies the name and extra-field
 * lengths needed to find where the data starts.
 */
std::expected<std::vector<ExtractedEntry>, std::string> ReadArchiveEntries(const std::string_view archive) {
    const auto endRecord = FindEndOfCentralDirectory(archive);
    if (!endRecord) {
        return std::unexpected("the archive has no end-of-central-directory record");
    }

    const auto entryCount = Read16(archive, *endRecord + 10);
    const auto directorySize = Read32(archive, *endRecord + 12);
    const auto directoryOffset = Read32(archive, *endRecord + 16);
    if (!entryCount || !directorySize || !directoryOffset) {
        return std::unexpected("the archive's end-of-central-directory record is truncated");
    }
    if (static_cast<std::size_t>(*directoryOffset) + *directorySize > archive.size()) {
        return std::unexpected("the archive's central directory lies outside the archive");
    }
    if (*entryCount > artifactMaxEntries) {
        return std::unexpected(std::format("the archive has more than {} entries", artifactMaxEntries));
    }

    std::vector<ExtractedEntry> entries;
    entries.reserve(*entryCount);
    std::size_t cursor = *directoryOffset;
    for (std::uint16_t index = 0; index < *entryCount; ++index) {
        if (Read32(archive, cursor) != 0x02014B50U) {
            return std::unexpected("the archive's central directory is malformed");
        }
        const auto method = Read16(archive, cursor + 10);
        const auto crc = Read32(archive, cursor + 16);
        const auto compressedSize = Read32(archive, cursor + 20);
        const auto storedSize = Read32(archive, cursor + 24);
        const auto nameLength = Read16(archive, cursor + 28);
        const auto extraLength = Read16(archive, cursor + 30);
        const auto commentLength = Read16(archive, cursor + 32);
        const auto localOffset = Read32(archive, cursor + 42);
        if (!method || !crc || !compressedSize || !storedSize || !nameLength || !extraLength || !commentLength ||
            !localOffset) {
            return std::unexpected("the archive's central directory is truncated");
        }
        if (cursor + 46 + *nameLength > archive.size()) {
            return std::unexpected("the archive's central directory is truncated");
        }

        std::string path(archive.substr(cursor + 46, *nameLength));
        cursor += 46 + static_cast<std::size_t>(*nameLength) + *extraLength + *commentLength;

        // A trailing slash marks a directory entry. `rux pack` never emits one,
        // and directories are implied by the file paths, so it is dropped.
        if (path.ends_with('/')) {
            continue;
        }
        if (*method != 0 || *compressedSize != *storedSize) {
            return std::unexpected(std::format("'{}' is compressed; package archives store their entries", path));
        }
        if (*storedSize > artifactMaxFileBytes) {
            return std::unexpected(std::format("'{}' exceeds the {}-byte file limit", path, artifactMaxFileBytes));
        }

        // The local header repeats the name and may carry a different extra
        // field, so the data offset has to be read from it rather than assumed.
        if (Read32(archive, *localOffset) != 0x04034B50U) {
            return std::unexpected(std::format("'{}' has no local file header", path));
        }
        const auto localNameLength = Read16(archive, *localOffset + 26);
        const auto localExtraLength = Read16(archive, *localOffset + 28);
        if (!localNameLength || !localExtraLength) {
            return std::unexpected(std::format("'{}' has a truncated local file header", path));
        }
        const std::size_t dataOffset =
            static_cast<std::size_t>(*localOffset) + 30 + *localNameLength + *localExtraLength;
        if (dataOffset + *storedSize > archive.size()) {
            return std::unexpected(std::format("'{}' extends past the end of the archive", path));
        }

        const std::string_view data = archive.substr(dataOffset, *storedSize);
        if (Crc32(data) != *crc) {
            return std::unexpected(std::format("'{}' failed its checksum", path));
        }
        entries.emplace_back(std::move(path), data);
    }
    return entries;
}
} // namespace

std::string ArtifactFileName(const Manifest &manifest) {
    return std::format("{}-{}{}", manifest.package.name.Text(), manifest.package.version.Text(), artifactExtension);
}

ArtifactResult BuildPackageArtifact(const fs::path &manifestPath, const Manifest &manifest) {
    const fs::path root = manifestPath.parent_path();

    auto manifestSource = ReadFileBytes(manifestPath);
    if (!manifestSource) {
        return std::unexpected(std::move(manifestSource.error()));
    }
    if (manifestSource->size() > manifestMaxBytes) {
        return std::unexpected(std::format("Rux.toml exceeds the {}-byte manifest limit", manifestMaxBytes));
    }
    if (!IsUtf8(*manifestSource)) {
        return std::unexpected("Rux.toml is not valid UTF-8");
    }

    std::vector<ArtifactEntry> entries;
    entries.emplace_back("Rux.toml", *manifestSource);
    if (auto collected = CollectSources(root, entries); !collected) {
        return std::unexpected(std::move(collected.error()));
    }

    // ReadmeFile and LicenseFile each name an archive entry, so a declared path
    // that is missing would fail remote validation. Reject it here instead.
    for (const std::string *declared : {&manifest.package.readmeFile, &manifest.package.licenseFile}) {
        const std::string &referenced = *declared;
        if (referenced.empty()) {
            continue;
        }
        std::error_code ec;
        const fs::path file = root / referenced;
        if (!fs::is_regular_file(file, ec)) {
            return std::unexpected(std::format("'{}' is declared in Rux.toml but is not a regular file", referenced));
        }
        auto data = ReadFileBytes(file);
        if (!data) {
            return std::unexpected(std::move(data.error()));
        }
        if (data->size() > artifactMaxTextBytes) {
            return std::unexpected(
                std::format("'{}' exceeds the {}-byte referenced-text limit", referenced, artifactMaxTextBytes));
        }
        if (!IsUtf8(*data)) {
            return std::unexpected(std::format("'{}' is not valid UTF-8", referenced));
        }
        entries.emplace_back(referenced, std::move(*data));
    }

    std::ranges::sort(entries, {}, &ArtifactEntry::path);

    std::size_t sourceFileCount = 0;
    std::size_t expandedBytes = 0;
    std::string previousFolded;
    for (const auto &entry : entries) {
        if (const std::string rejection = RejectEntryPath(entry.path); !rejection.empty()) {
            return std::unexpected(rejection);
        }
        if (entry.data.size() > artifactMaxFileBytes) {
            return std::unexpected(
                std::format("'{}' exceeds the {}-byte file limit", entry.path, artifactMaxFileBytes));
        }
        if (IsRuxSource(entry.path)) {
            if (!IsUtf8(entry.data)) {
                return std::unexpected(std::format("'{}' is not valid UTF-8", entry.path));
            }
            ++sourceFileCount;
        }
        expandedBytes += entry.data.size();

        // Sorted order puts colliding spellings next to each other.
        std::string folded = entry.path;
        std::ranges::transform(folded, folded.begin(),
                               [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (folded == previousFolded) {
            return std::unexpected(
                std::format("'{}' collides with another entry that differs only in case", entry.path));
        }
        previousFolded = std::move(folded);
    }

    if (sourceFileCount == 0) {
        return std::unexpected("the package contains no Src/**/*.rux source file");
    }
    if (entries.size() > artifactMaxEntries) {
        return std::unexpected(std::format("the package has more than {} entries", artifactMaxEntries));
    }
    if (expandedBytes > artifactMaxExpandedBytes) {
        return std::unexpected(std::format("the package expands to more than {} bytes", artifactMaxExpandedBytes));
    }

    std::string archive;
    std::string directory;
    archive.reserve(expandedBytes + entries.size() * 128);
    for (const auto &entry : entries) {
        WriteEntry(entry, archive, directory);
    }

    const auto directoryOffset = static_cast<std::uint32_t>(archive.size());
    const auto entryCount = static_cast<std::uint16_t>(entries.size());
    archive.append(directory);
    const auto put16 = [&archive](const std::uint16_t value) {
        archive.push_back(static_cast<char>(value & 0xFFU));
        archive.push_back(static_cast<char>((value >> 8U) & 0xFFU));
    };
    const auto put32 = [&archive](const std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            archive.push_back(static_cast<char>((value >> shift) & 0xFFU));
        }
    };
    put32(0x06054B50); // End of central directory signature
    put16(0);          // Number of this disk
    put16(0);          // Disk with the start of the central directory
    put16(entryCount);
    put16(entryCount);
    put32(static_cast<std::uint32_t>(directory.size()));
    put32(directoryOffset);
    put16(0); // Archive comment length

    if (archive.size() > artifactMaxBytes) {
        return std::unexpected(std::format("the archive exceeds the {}-byte publication limit", artifactMaxBytes));
    }

    return PackageArtifact{.manifestSource = std::move(*manifestSource),
                           .archive = std::move(archive),
                           .fileCount = entries.size(),
                           .sourceFileCount = sourceFileCount};
}

std::expected<ExtractedArtifact, std::string> ExtractPackageArtifact(const std::string_view archive,
                                                                     const fs::path &dest) {
    if (archive.size() > artifactMaxBytes) {
        return std::unexpected(std::format("the archive exceeds the {}-byte publication limit", artifactMaxBytes));
    }

    auto entries = ReadArchiveEntries(archive);
    if (!entries) {
        return std::unexpected(std::move(entries.error()));
    }

    ExtractedArtifact summary;
    bool foundManifest = false;
    std::vector<std::string> folded;
    folded.reserve(entries->size());
    for (const auto &entry : *entries) {
        if (const std::string rejection = RejectEntryPath(entry.path); !rejection.empty()) {
            return std::unexpected(rejection);
        }
        if (entry.path == "Rux.toml") {
            if (entry.data.size() > manifestMaxBytes) {
                return std::unexpected(std::format("Rux.toml exceeds the {}-byte manifest limit", manifestMaxBytes));
            }
            if (!IsUtf8(entry.data)) {
                return std::unexpected("Rux.toml is not valid UTF-8");
            }
            foundManifest = true;
        }
        if (IsRuxSource(entry.path)) {
            if (!IsUtf8(entry.data)) {
                return std::unexpected(std::format("'{}' is not valid UTF-8", entry.path));
            }
            ++summary.sourceFileCount;
        }
        summary.expandedBytes += entry.data.size();
        if (summary.expandedBytes > artifactMaxExpandedBytes) {
            return std::unexpected(std::format("the package expands to more than {} bytes", artifactMaxExpandedBytes));
        }

        std::string key = entry.path;
        std::ranges::transform(key, key.begin(),
                               [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        folded.push_back(std::move(key));
    }

    std::ranges::sort(folded);
    if (std::ranges::adjacent_find(folded) != folded.end()) {
        return std::unexpected("the archive has entries that differ only in case");
    }
    if (!foundManifest) {
        return std::unexpected("the archive has no Rux.toml at its root");
    }
    if (summary.sourceFileCount == 0) {
        return std::unexpected("the archive contains no Src/**/*.rux source file");
    }
    summary.fileCount = entries->size();

    // Every entry has been accepted, so writing can start. A failure from here
    // is a filesystem problem; the caller discards the staging directory.
    std::error_code ec;
    fs::create_directories(dest, ec);
    if (ec) {
        return std::unexpected(std::format("failed to create '{}'", dest.generic_string()));
    }
    for (const auto &entry : *entries) {
        const fs::path output = dest / fs::path(entry.path);
        fs::create_directories(output.parent_path(), ec);
        if (ec) {
            return std::unexpected(std::format("failed to create '{}'", output.parent_path().generic_string()));
        }
        std::ofstream file(output, std::ios::binary | std::ios::trunc);
        if (!file.write(entry.data.data(), static_cast<std::streamsize>(entry.data.size()))) {
            return std::unexpected(std::format("failed to write '{}'", output.generic_string()));
        }
    }
    return summary;
}
} // namespace Rux
