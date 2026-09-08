// The language test matrix: every primitive and every keyword the compiler knows has somewhere to be exercised.
//
// Tests/Language is organized one directory per language feature, and for the primitive types one directory per
// width. That layout is only worth having if it stays complete, and completeness is exactly the property that decays
// silently: a width added to PrimitiveCatalog.cpp compiles, lowers and ships without anyone noticing that nothing
// runs it. That is how int512 and uint256 came to be implemented with no executable coverage at all, and how
// `extern` came to have no language test while being used by five packages.
//
// So the compiler's own tables are the authority — PrimitiveCatalog.cpp for the types and Token.cpp for the
// keywords — and this file compares what exists against them rather than against lists copied out of them. A copied
// list would drift exactly the way the directories did.
//
// The two halves check different things, because the two are shaped differently. A primitive owns a directory named
// after it, so that correspondence is computed. A keyword does not: `do` is exercised by DoWhile, `pub` by
// Visibility, `import` and `module` both by ModuleScope, and words like `as`, `in` and `is` appear in prose
// everywhere, so no word search could establish ownership. Keyword ownership is therefore written out and checked,
// which also means adding a keyword to the language forces someone to say which case covers it.
//
// A reserved width gets a directory too, holding a placeholder that says so. It cannot hold a program that uses the
// type, because such a program does not compile; the refusal is asserted in Golden/ReservedPrimitiveTypes.rux. The
// placeholder exists so the matrix is complete by inspection and so an implemented width arrives at a directory
// that already exists.

#include "Lexer/Token.h"
#include "Types/PrimitiveCatalog.h"

#include <algorithm>
#include <cctype>
#include <doctest.h>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>

using namespace Rux;

namespace {

/// The directory a primitive's cases live in: its canonical spelling with the first letter capitalized, which is how
/// Int8, Uint512, Char64 and Float80 are already named.
std::string DirectoryFor(const std::string_view primitive) {
    std::string name(primitive);
    if (!name.empty()) {
        name.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(name.front())));
    }
    return name;
}

std::filesystem::path LanguageTestsRoot() {
    return std::filesystem::path(RUX_TESTS_DIR) / "Language";
}

/// Every immediate subdirectory of Tests/Language that is a package, meaning it has a manifest. A stray directory
/// without one is not a test and must not satisfy a coverage requirement.
std::set<std::string> LanguageTestDirectories() {
    std::set<std::string> directories;
    for (const auto &entry : std::filesystem::directory_iterator(LanguageTestsRoot())) {
        if (entry.is_directory() && std::filesystem::exists(entry.path() / "Rux.toml")) {
            directories.insert(entry.path().filename().string());
        }
    }
    return directories;
}

} // namespace

TEST_CASE("Every primitive in the catalog has a language test directory") {
    const auto directories = LanguageTestDirectories();
    REQUIRE_MESSAGE(directories.size() > 1, "no language tests found below ", LanguageTestsRoot().string());

    for (const auto &primitive : PrimitiveCatalog()) {
        const auto directory = DirectoryFor(primitive.name);
        CHECK_MESSAGE(directories.contains(directory), "primitive '", primitive.name, "' has no Tests/Language/",
                      directory,
                      " directory. Add one: an implemented width gets a real case, a reserved width gets the "
                      "placeholder the other reserved widths use.");
    }
}

TEST_CASE("Every primitive test directory names a primitive the catalog has") {
    // The other direction. A directory left behind by a removed or renamed width would otherwise sit there looking
    // like coverage. Only directories whose names match a catalog spelling are considered, so the feature tests
    // (Struct, Iteration, Propagation and the rest) are untouched by this.
    std::set<std::string> expected;
    for (const auto &primitive : PrimitiveCatalog()) {
        expected.insert(DirectoryFor(primitive.name));
    }

    for (const auto &directory : LanguageTestDirectories()) {
        std::string lowered = directory;
        std::ranges::transform(lowered, lowered.begin(),
                               [](unsigned char character) { return static_cast<char>(std::tolower(character)); });

        // Only a canonical spelling claims a per-width directory. An alias does not: `bool`, `byte`, `char` and
        // `float` each resolve to a width that already has its own directory, so Tests/Language/Bool is free to be
        // the boolean-logic case rather than bool8's. BoolMethod and the other feature tests resolve to nothing.
        if (FindPrimitive(lowered) == nullptr || CanonicalPrimitiveName(lowered) != lowered) {
            continue;
        }
        CHECK_MESSAGE(expected.contains(directory), "Tests/Language/", directory,
                      " names a primitive spelling the catalog does not have as a canonical name. Rename it to the "
                      "canonical spelling or remove it.");
    }
}

namespace {

/// Which language test owns each keyword.
///
/// Written out rather than derived. Ownership cannot be inferred from a word search: `as`, `in`, `is` and `do` all
/// occur inside ordinary prose and identifiers, so counting files that mention a keyword measures nothing. Nor does
/// a keyword map to a directory of its own name — `do` is exercised by DoWhile, `pub` by Visibility, `type` by
/// TypeAlias, `import` and `module` by ModuleScope. Naming the owner makes the claim checkable: the directory must
/// exist, and a keyword added to the lexer must be given an owner here before this compiles as coverage.
const std::map<std::string_view, std::string_view> kKeywordOwners = {
    {"as", "As"},
    {"break", "Break"},
    {"const", "Const"},
    {"continue", "Continue"},
    {"defer", "Defer"},
    {"do", "DoWhile"},
    {"else", "If"},
    {"enum", "Enum"},
    {"extend", "SliceExtend"},
    {"extern", "Extern"},
    {"false", "Bool"},
    {"for", "For"},
    {"func", "Functions"},
    {"if", "If"},
    {"import", "ModuleScope"},
    {"in", "For"},
    {"interface", "Interface"},
    {"intrinsic", "IntrinsicReplacement"},
    {"is", "Is"},
    {"let", "Var"},
    {"loop", "Loop"},
    {"match", "Match"},
    {"module", "ModuleScope"},
    {"null", "Pointers"},
    {"pub", "Visibility"},
    {"return", "Functions"},
    {"self", "Receiver"},
    {"struct", "Struct"},
    {"true", "Bool"},
    {"type", "TypeAlias"},
    {"union", "Union"},
    {"var", "Var"},
    {"variant", "Variant"},
    {"when", "When"},
    {"while", "While"},
};

} // namespace

TEST_CASE("Every keyword the lexer recognizes has a named owning language test") {
    const auto directories = LanguageTestDirectories();

    for (const auto &keyword : Keywords()) {
        const auto owner = kKeywordOwners.find(keyword);
        REQUIRE_MESSAGE(owner != kKeywordOwners.end(), "keyword '", keyword,
                        "' has no owning language test. Add one and name it in kKeywordOwners, or point the keyword "
                        "at the existing case that exercises it.");
        CHECK_MESSAGE(directories.contains(std::string(owner->second)), "keyword '", keyword, "' names Tests/Language/",
                      owner->second,
                      " as its owner, and no such directory exists. Update kKeywordOwners after a rename.");
    }
}

TEST_CASE("The keyword ownership table names no keyword the lexer does not have") {
    // The other direction, so a keyword removed from the language does not leave a stale claim of coverage behind.
    for (const auto &[keyword, owner] : kKeywordOwners) {
        CHECK_MESSAGE(KeywordKind(keyword) != TokenKind::Ident, "kKeywordOwners names '", keyword,
                      "', which the lexer does not treat as a keyword. Remove the entry.");
    }
    CHECK_MESSAGE(kKeywordOwners.size() == Keywords().size(),
                  "the ownership table and the lexer's keyword table are different sizes");
}

TEST_CASE("The catalog's implemented and reserved counts are what the matrix was built against") {
    // Not a rule, a tripwire. These numbers are here so that adding or implementing a width fails this case and
    // sends whoever did it to the directory that now needs writing, rather than leaving the matrix quietly stale.
    std::size_t implemented = 0;
    std::size_t reserved = 0;
    for (const auto &primitive : PrimitiveCatalog()) {
        if (primitive.implemented) {
            ++implemented;
        }
        else {
            ++reserved;
        }
    }

    CHECK_MESSAGE(implemented == 26,
                  "the implemented primitive count changed. A newly implemented width needs its placeholder in "
                  "Tests/Language replaced with a real case, and its line removed from "
                  "Golden/ReservedPrimitiveTypes.expected.");
    CHECK_MESSAGE(reserved == 12,
                  "the reserved primitive count changed. A newly reserved width needs a Tests/Language placeholder "
                  "and a line in Golden/ReservedPrimitiveTypes.rux.");
    CHECK(implemented + reserved == PrimitiveCatalog().size());
}
