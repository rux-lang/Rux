#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace Rux {
/// The stable identity and contents of one source file. Loading policy belongs to SourceLoader; compiler stages may
/// consume this value without depending on filesystem traversal or diagnostics.
struct SourceFile {
    std::filesystem::path path; ///< absolute path to the file
    std::string source;         ///< full file contents
    /// The file's size and last write time, taken before its contents were read, so an edit made after the stamp
    /// shows as a newer one. A stamp that could not be taken, or a file that was never on disk, keeps these defaults.
    std::uintmax_t size = ~std::uintmax_t{0};
    std::filesystem::file_time_type modified{};
};
} // namespace Rux
