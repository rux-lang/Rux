#pragma once

// What a successful build read and wrote, recorded so the next build with the same inputs can reuse its artifact
// instead of compiling again.
//
// A record names the request (`key`: the compiler executable, the options and the environment a build depends on),
// every source file and manifest the build read, the listing of every directory sources were discovered in, and the
// artifacts it produced. A later build is up to date only when all of them still match: the key is equal, every input
// has the same content (its size and modification time, or failing those its hash), every directory lists the same
// entries, and every artifact is still the file the build wrote. Anything else, including a missing or unreadable
// record, rebuilds.

#include "Driver/BuildStats.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Rux::Driver {
struct BuildRecord {
    struct File {
        std::filesystem::path path;
        std::uintmax_t size = 0;
        std::int64_t modified = 0;
        std::string hash;
    };

    struct Directory {
        std::filesystem::path path;
        std::string listing;
        /// Whether the listing covers subdirectories too, as source discovery does, or only direct entries.
        bool recursive = true;
    };

    std::string key;
    std::vector<File> inputs;
    std::vector<Directory> directories;
    std::vector<File> artifacts;
    /// Source counts of the recorded build, reported again when its artifact is reused.
    BuildStats stats;
};

/// A digest of `text`, as stored in a record.
[[nodiscard]] std::string HashText(std::string_view text);

/// The record of `path` as it is now: its size, modification time and content hash. Nullopt when it cannot be read.
[[nodiscard]] std::optional<BuildRecord::File> RecordFile(const std::filesystem::path &path);

/// The record of a file whose content the build already holds, without reading it again.
[[nodiscard]] std::optional<BuildRecord::File> RecordFile(const std::filesystem::path &path, std::string_view content);

/// A digest of the entries under `directory`, recursively, by relative path and kind. Empty when it does not exist.
[[nodiscard]] std::string DirectoryListing(const std::filesystem::path &directory, bool recursive);

/// Whether every input, directory and artifact `record` names is unchanged.
[[nodiscard]] bool IsUpToDate(const BuildRecord &record);

[[nodiscard]] std::optional<BuildRecord> ReadBuildRecord(const std::filesystem::path &path);

/// Write `record` to `path`, replacing any earlier one in a single rename. Returns false when it cannot be written.
bool WriteBuildRecord(const std::filesystem::path &path, const BuildRecord &record);
} // namespace Rux::Driver
