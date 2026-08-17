#pragma once

#include <filesystem>
#include <string>

namespace Rux {
/// The stable identity and contents of one source file. Loading policy belongs to SourceLoader; compiler stages may
/// consume this value without depending on filesystem traversal or diagnostics.
struct SourceFile {
    std::filesystem::path path; ///< absolute path to the file
    std::string source;         ///< full file contents
};
} // namespace Rux
