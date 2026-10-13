#include "Driver/BuildCache.h"

#include "Crypto/Sha256.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <fstream>
#include <sstream>
#include <system_error>

namespace Rux::Driver {
namespace {
constexpr std::string_view kRecordHeader = "rux-build-record 1";

std::string Hex(const Crypto::Sha256Digest &digest) {
    std::string text;
    text.reserve(digest.size() * 2);
    for (const auto byte : digest)
        text += std::format("{:02x}", byte);
    return text;
}

std::int64_t ModificationTicks(const std::filesystem::path &path, std::error_code &error) {
    const auto time = std::filesystem::last_write_time(path, error);
    return error ? 0 : static_cast<std::int64_t>(time.time_since_epoch().count());
}

std::optional<std::string> ReadWholeFile(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return std::nullopt;
    std::ostringstream content;
    content << stream.rdbuf();
    if (stream.bad())
        return std::nullopt;
    return std::move(content).str();
}

std::string PathText(const std::filesystem::path &path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

std::filesystem::path TextPath(const std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

/// The fields of `line` up to `count`, the last running to the end of the line so it may hold a path with spaces.
std::vector<std::string_view> SplitFields(const std::string_view line, const std::size_t count) {
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (fields.size() + 1 < count) {
        const auto space = line.find(' ', start);
        if (space == std::string_view::npos)
            break;
        fields.push_back(line.substr(start, space - start));
        start = space + 1;
    }
    fields.push_back(line.substr(start));
    return fields;
}

template <typename Number>
bool ParseNumber(const std::string_view text, Number &value) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

/// The counts a record keeps, by the name it stores them under.
template <typename Visit>
void VisitCounts(BuildStats &stats, Visit visit) {
    visit("localFiles", stats.localFiles);
    visit("dependencyFiles", stats.dependencyFiles);
    visit("localLines", stats.localLines);
    visit("dependencyLines", stats.dependencyLines);
    visit("localTokens", stats.localTokens);
    visit("dependencyTokens", stats.dependencyTokens);
    visit("localSourceSize", stats.localSourceSize);
    visit("dependencySourceSize", stats.dependencySourceSize);
    visit("executableSize", stats.executableSize);
}

bool SameFile(const BuildRecord::File &recorded, const bool compareContent) {
    std::error_code error;
    const auto size = std::filesystem::file_size(recorded.path, error);
    if (error || size != recorded.size)
        return false;
    const auto modified = ModificationTicks(recorded.path, error);
    if (error)
        return false;
    if (modified == recorded.modified)
        return true;
    // Touched but perhaps not changed: the content decides.
    if (!compareContent || recorded.hash.empty())
        return false;
    const auto content = ReadWholeFile(recorded.path);
    return content && HashText(*content) == recorded.hash;
}
} // namespace

std::string HashText(const std::string_view text) {
    return Hex(Crypto::Sha256({reinterpret_cast<const std::uint8_t *>(text.data()), text.size()}));
}

std::optional<BuildRecord::File> RecordFile(const std::filesystem::path &path) {
    const auto content = ReadWholeFile(path);
    if (!content)
        return std::nullopt;
    return RecordFile(path, *content);
}

std::optional<BuildRecord::File> RecordFile(const std::filesystem::path &path, const std::string_view content) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error)
        return std::nullopt;
    const auto modified = ModificationTicks(path, error);
    if (error)
        return std::nullopt;
    return BuildRecord::File{path, size, modified, HashText(content)};
}

std::string DirectoryListing(const std::filesystem::path &directory, const bool recursive) {
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error))
        return {};
    std::vector<std::string> entries;
    const auto add = [&](const std::filesystem::directory_entry &entry) {
        std::error_code kindError;
        const char kind = entry.is_directory(kindError) ? 'd' : 'f';
        entries.push_back(std::format("{} {}", kind, PathText(entry.path().lexically_relative(directory))));
    };
    if (recursive) {
        for (std::filesystem::recursive_directory_iterator it(directory, error), end; !error && it != end;
             it.increment(error))
            add(*it);
    }
    else {
        for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error))
            add(*it);
    }
    if (error)
        return "unreadable";
    std::ranges::sort(entries);
    std::string joined;
    for (const auto &entry : entries) {
        joined += entry;
        joined += '\n';
    }
    return HashText(joined);
}

bool IsUpToDate(const BuildRecord &record) {
    for (const auto &artifact : record.artifacts)
        if (!SameFile(artifact, false))
            return false;
    for (const auto &directory : record.directories)
        if (DirectoryListing(directory.path, directory.recursive) != directory.listing)
            return false;
    for (const auto &input : record.inputs)
        if (!SameFile(input, true))
            return false;
    return true;
}

std::optional<BuildRecord> ReadBuildRecord(const std::filesystem::path &path) {
    const auto content = ReadWholeFile(path);
    if (!content)
        return std::nullopt;
    std::istringstream lines(*content);
    std::string line;
    if (!std::getline(lines, line) || line != kRecordHeader)
        return std::nullopt;
    BuildRecord record;
    while (std::getline(lines, line)) {
        const auto kind = line.substr(0, line.find(' '));
        if (kind == "key") {
            record.key = line.substr(4);
        }
        else if (kind == "stat") {
            const auto fields = SplitFields(line, 3);
            if (fields.size() != 3)
                return std::nullopt;
            bool known = false;
            VisitCounts(record.stats, [&](const std::string_view name, auto &member) {
                if (name == fields[1])
                    known = ParseNumber(fields[2], member);
            });
            if (!known)
                return std::nullopt;
        }
        else if (kind == "input" || kind == "artifact") {
            const auto fields = SplitFields(line, 5);
            BuildRecord::File file;
            if (fields.size() != 5 || !ParseNumber(fields[1], file.size) || !ParseNumber(fields[2], file.modified))
                return std::nullopt;
            file.hash = fields[3] == "-" ? std::string{} : std::string(fields[3]);
            file.path = TextPath(fields[4]);
            (kind == "input" ? record.inputs : record.artifacts).push_back(std::move(file));
        }
        else if (kind == "directory") {
            const auto fields = SplitFields(line, 4);
            if (fields.size() != 4 || (fields[1] != "r" && fields[1] != "1"))
                return std::nullopt;
            record.directories.push_back(
                {TextPath(fields[3]), fields[2] == "-" ? std::string{} : std::string(fields[2]), fields[1] == "r"});
        }
        else if (!line.empty()) {
            return std::nullopt;
        }
    }
    return record;
}

bool WriteBuildRecord(const std::filesystem::path &path, const BuildRecord &record) {
    std::string text(kRecordHeader);
    text += '\n';
    text += std::format("key {}\n", record.key);
    auto stats = record.stats;
    VisitCounts(stats, [&](const std::string_view name, const auto &member) {
        text += std::format("stat {} {}\n", name, member);
    });
    for (const auto &input : record.inputs)
        text += std::format("input {} {} {} {}\n", input.size, input.modified, input.hash.empty() ? "-" : input.hash,
                            PathText(input.path));
    for (const auto &directory : record.directories)
        text += std::format("directory {} {} {}\n", directory.recursive ? "r" : "1",
                            directory.listing.empty() ? "-" : directory.listing, PathText(directory.path));
    for (const auto &artifact : record.artifacts)
        text += std::format("artifact {} {} - {}\n", artifact.size, artifact.modified, PathText(artifact.path));

    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error)
        return false;
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream)
            return false;
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!stream.good())
            return false;
    }
    std::filesystem::rename(temporary, path, error);
    return !error;
}
} // namespace Rux::Driver
