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
#include <ranges>
#include <string_view>
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

    // Readme names an archive entry, so a declared path that is missing would
    // fail remote validation. Reject it here instead.
    if (const std::string &referenced = manifest.package.readme; !referenced.empty()) {
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
} // namespace Rux
