#pragma once

// The fenced Rux blocks of the first-party package READMEs, written out as scratch packages the real compiler can
// check. PackageExampleTests compiles each one.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Rux::Testing::PackageExamples {

/// What a fenced block turned out to be. Only programs, declarations, and examples are the compiler's business.
enum class Kind : std::uint8_t {
    Signature,
    Program,
    Declaration,
    Example,
    Fragment
};

[[nodiscard]] std::string_view Describe(Kind kind);

/// One README block written out as a scratch package.
struct ReadmeExample {
    std::string package;            ///< the first-party package whose README holds the block
    std::size_t block = 0;          ///< one-based position of the block in that README
    std::string name;               ///< package name plus position, as in `Collections3`
    Kind kind = Kind::Example;      ///< how the block was read
    std::string source;             ///< the compilable source written for it
    std::filesystem::path manifest; ///< the scratch package's Rux.toml
    std::string_view target;        ///< the target it is checked for, or empty for the host
    bool fragment = false;          ///< named in Fragments: it leans on prose and is not expected to compile
};

/// The blocks that lean on names their surrounding prose supplies, by package and position. The list may only shrink.
[[nodiscard]] std::span<const std::string_view> Fragments();

/// Writes every README block except lone signatures under `root` as a scratch package, in a stable order, and
/// returns them with the number of signatures skipped.
[[nodiscard]] std::vector<ReadmeExample> MaterializeReadmeExamples(const std::filesystem::path &root,
                                                                   std::size_t &signatures);

} // namespace Rux::Testing::PackageExamples
