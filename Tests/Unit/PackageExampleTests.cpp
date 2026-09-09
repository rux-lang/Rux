// Every fenced Rux block in a first-party package README, compiled by the real compiler.
//
// Documentation examples are the one part of a package nothing else checks. `rux check` never sees a README, the
// documentation-style suite reads declarations rather than prose, and a reader is otherwise the first to discover
// that a snippet does not build. Five did not: an `import Core::#target;` that does not parse, repeated in all four
// platform READMEs; two Algorithms examples written against a pointer-and-length API that slices replaced; a C
// example passing a literal where a `*char8` is wanted; an Io example omitting a required type argument and passing
// a concrete stream where an interface is expected; and a Collections example whose `Debug` bound cannot be
// satisfied without importing `Rux/Format`. Nothing in the suite would have caught any of them.
//
// A fenced block is one of four things, and only the first three are the compiler's business.
//
//   * A whole program, recognized by its own `func Main`.
//   * A run of statements, which goes inside a generated `Main`.
//   * Declarations with nothing to run, which get an empty `Main`.
//   * A fragment leaning on names the surrounding prose supplies, which cannot compile alone and never claimed to.
//
// A fragment cannot be recognized from what the compiler says. Any block that leans on a name would report an
// undefined one, so a rule keyed on that diagnostic excuses exactly the regression worth catching: an example that
// comes to name something the package no longer has. The fragments are therefore named here, and the list may only
// shrink — a named block that starts compiling fails, because it has become an example and must be checked as one.

#include "System/Os.h"
#include "System/Process.h"

#include <algorithm>
#include <array>
#include <doctest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using namespace Rux;

namespace {

// --- Source text ----------------------------------------------------------------------------------------------

std::string ReadFileText(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void WriteFileText(const std::filesystem::path &path, const std::string_view contents) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    REQUIRE(!error);
    std::ofstream output(path, std::ios::binary);
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

/// The README lines, with any carriage returns dropped so that a rule never has to look for one.
std::vector<std::string> SplitLines(const std::string_view text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find('\n', start);
        auto line = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        lines.emplace_back(line);
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return lines;
}

std::string_view TrimLeft(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    return text;
}

std::string_view Trim(std::string_view text) {
    text = TrimLeft(text);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

/// True when `text` opens with `word` as a whole word rather than as the head of a longer identifier.
bool OpensWith(const std::string_view text, const std::string_view word) {
    if (!text.starts_with(word)) {
        return false;
    }
    const auto rest = text.substr(word.size());
    return rest.empty() || rest.front() == ' ' || rest.front() == '\t' || rest.front() == '(' || rest.front() == '<';
}

/// The line with a leading `pub` removed, so that a rule states the shape once rather than twice.
std::string_view WithoutVisibility(std::string_view line) {
    line = TrimLeft(line);
    if (OpensWith(line, "pub")) {
        line = TrimLeft(line.substr(3));
    }
    return line;
}

// --- Blocks ---------------------------------------------------------------------------------------------------

/// Every first-party package holding a README, in a stable order.
std::vector<std::pair<std::string, std::filesystem::path>> PackagesWithReadme() {
    std::vector<std::pair<std::string, std::filesystem::path>> packages;
    const auto root = std::filesystem::weakly_canonical(std::filesystem::path(RUX_PACKAGES_DIR));
    for (const auto &entry : std::filesystem::directory_iterator(root)) {
        if (entry.is_directory() && std::filesystem::exists(entry.path() / "README.md")) {
            packages.emplace_back(entry.path().filename().string(), entry.path());
        }
    }
    std::ranges::sort(packages);
    return packages;
}

/// The `rux` fences of one README, in the order a reader meets them.
std::vector<std::string> FencedBlocks(const std::string_view text) {
    std::vector<std::string> blocks;
    const auto lines = SplitLines(text);
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (Trim(lines[index]) != "```rux") {
            continue;
        }
        std::string body;
        for (++index; index < lines.size() && Trim(lines[index]) != "```"; ++index) {
            body += lines[index];
            body += '\n';
        }
        blocks.push_back(body);
    }
    return blocks;
}

// --- Classification -------------------------------------------------------------------------------------------

enum class Kind : std::uint8_t {
    Signature,
    Program,
    Declaration,
    Example,
    Fragment
};

std::string_view Describe(const Kind kind) {
    switch (kind) {
    case Kind::Signature:
        return "signature";
    case Kind::Program:
        return "program";
    case Kind::Declaration:
        return "declaration";
    case Kind::Example:
        return "example";
    case Kind::Fragment:
        return "fragment";
    }
    return "unknown";
}

/// True for a line opening a file-scope declaration, which has to stay outside the generated `Main`.
bool OpensDeclaration(const std::string_view line) {
    static constexpr std::array keywords{"when",      "func",   "struct", "enum", "variant", "union",
                                         "interface", "extend", "const",  "type", "module"};
    const auto text = WithoutVisibility(line);
    return std::ranges::any_of(keywords, [text](const std::string_view word) { return OpensWith(text, word); });
}

/// True for a lone signature shown to illustrate a shape. There is nothing to run and nothing to check.
bool IsSignatureOnly(const std::string_view line) {
    const auto text = WithoutVisibility(line);
    return text.starts_with("func ") && text.find('{') == std::string_view::npos &&
           text.find(';') == std::string_view::npos;
}

/// A block wrapped into a compilable source, and what kind of block it turned out to be.
struct Candidate {
    Kind kind = Kind::Example;
    std::string source;
};

Candidate Classify(const std::string_view body) {
    auto lines = SplitLines(body);
    while (!lines.empty() && Trim(lines.back()).empty()) {
        lines.pop_back();
    }

    std::vector<std::string> meaningful;
    for (const auto &line : lines) {
        const auto text = Trim(line);
        if (!text.empty() && !text.starts_with("//")) {
            meaningful.emplace_back(text);
        }
    }
    if (meaningful.size() == 1 && IsSignatureOnly(meaningful.front())) {
        return {Kind::Signature, std::string(body)};
    }
    if (body.find("func Main") != std::string_view::npos) {
        return {Kind::Program, std::string(body)};
    }

    // Imports have to lead the file and declarations have to stay at file scope, so the block is split into three
    // runs: leading imports, whole declarations tracked by brace depth, and everything else.
    std::vector<std::string> imports;
    std::vector<std::string> declarations;
    std::vector<std::string> statements;
    int depth = 0;
    bool collecting = false;
    for (const auto &line : lines) {
        if (depth == 0 && !collecting && line.starts_with("import ")) {
            imports.push_back(line);
            continue;
        }
        if (depth == 0 && !collecting && OpensDeclaration(line)) {
            collecting = true;
        }
        (collecting ? declarations : statements).push_back(line);
        depth += static_cast<int>(std::ranges::count(line, '{')) - static_cast<int>(std::ranges::count(line, '}'));
        if (collecting && depth == 0) {
            collecting = false;
        }
    }

    std::string head;
    for (const auto &line : imports) {
        head += line + "\n";
    }
    if (!imports.empty()) {
        head += "\n";
    }
    if (std::ranges::any_of(declarations, [](const std::string_view line) { return !Trim(line).empty(); })) {
        for (const auto &line : declarations) {
            head += line + "\n";
        }
        head += "\n";
    }

    if (std::ranges::none_of(statements, [](const std::string_view line) { return !Trim(line).empty(); })) {
        return {Kind::Declaration, head + "func Main() -> int {\n    return 0;\n}\n"};
    }

    std::string indented;
    bool yieldsNothing = false;
    for (const auto &line : statements) {
        const auto text = Trim(line);
        if (!text.empty()) {
            indented += "    ";
            indented += line;
            // A block ending a function that yields nothing needs one, rather than an `int` it cannot satisfy.
            yieldsNothing = yieldsNothing || text == "return;";
        }
        indented += "\n";
    }
    if (yieldsNothing) {
        return {Kind::Example, head + "func Main() {\n" + indented + "}\n"};
    }
    return {Kind::Example, head + "func Main() -> int {\n" + indented + "    return 0;\n}\n"};
}

/// The blocks that lean on names their surrounding prose supplies, by package and position. Each one is a
/// deliberate choice by the README's author to show a call in the middle of an explanation rather than a program
/// around it, so each is named rather than guessed at. The list may only shrink.
constexpr std::array Fragments{
    "Collections3", // `Vector` and `sink` are the running example the section builds up.
    "Collections4", // The same, continued.
    "FileSystem1",  // `WriteAll(file, contents)` shown against the file the prose has just opened.
    "Json1",        // A `match` whose parse and handlers the text describes.
    "Toml1",        // The same shape, over a document the text names.
};

// --- The scratch package --------------------------------------------------------------------------------------

/// A platform package's example is guarded by `when #target.os` with no arm for the host, so it has to be checked
/// for the target it is about rather than the one the test happens to run on.
std::string_view TargetFor(const std::string_view package) {
    if (package == "Linux") {
        return "linux-x86_64";
    }
    if (package == "macOS") {
        return "macos-x86_64";
    }
    if (package == "FreeBSD") {
        return "freebsd-x86_64";
    }
    return {};
}

/// Every first-party package is listed as a dependency, so a block importing a neighbour still resolves and no
/// example has to be read twice to work out what it needs. A manifest may only name a dependency by a
/// relative path, so the table is written against the scratch package that will hold it.
std::string DependencyTable(const std::filesystem::path &scratch) {
    std::string table;
    const auto root = std::filesystem::weakly_canonical(std::filesystem::path(RUX_PACKAGES_DIR));
    std::vector<std::string> names;
    for (const auto &entry : std::filesystem::directory_iterator(root)) {
        if (entry.is_directory() && std::filesystem::exists(entry.path() / "Rux.toml")) {
            names.push_back(entry.path().filename().string());
        }
    }
    std::ranges::sort(names);
    for (const auto &name : names) {
        table += name + " = { Path = \"" + std::filesystem::relative(root / name, scratch).generic_string() + "\" }\n";
    }
    return table;
}

} // namespace

TEST_CASE("every package README example compiles") {
    const auto compiler = std::filesystem::path(RUX_ROOT_DIR) / "Bin" / System::ExecutableFileName("rux");
    REQUIRE_MESSAGE(std::filesystem::exists(compiler), "the compiler has to be built before its examples are checked");

    const auto root = std::filesystem::path(RUX_TEST_BIN_DIR) / "rux-readme-examples";
    std::filesystem::remove_all(root);
    std::error_code created;
    std::filesystem::create_directories(root / "Bin", created);
    REQUIRE(!created);

    std::size_t compilable = 0;
    std::size_t signatures = 0;
    std::vector<std::string> seenFragments;
    for (const auto &[package, directory] : PackagesWithReadme()) {
        const auto blocks = FencedBlocks(ReadFileText(directory / "README.md"));
        for (std::size_t index = 0; index < blocks.size(); ++index) {
            const auto name = package + std::to_string(index + 1);
            auto [kind, source] = Classify(blocks[index]);
            if (kind == Kind::Signature) {
                ++signatures;
                continue;
            }

            const auto scratch = root / name;
            std::filesystem::create_directories(scratch, created);
            REQUIRE(!created);
            const auto dependencies = DependencyTable(scratch);
            WriteFileText(scratch / "Src" / "Main.rux", source);
            WriteFileText(scratch / "Rux.toml", "[Manifest]\nVersion = 1\n\n[Package]\nName = \"" + name +
                                                    "\"\nVersion = \"0.1.0\"\n"
                                                    "Type = \"Executable\"\nDescription = \"README example " +
                                                    std::to_string(index + 1) + " from Rux/" + package +
                                                    "\"\n\n[Dependencies]\n" + dependencies + "\n[Build]\nOutput = \"" +
                                                    std::filesystem::relative(root / "Bin", scratch).generic_string() +
                                                    "\"\n");

            const auto manifest = (scratch / "Rux.toml").string();
            const auto target = TargetFor(package);
            std::vector<std::string_view> arguments{"--manifest", manifest, "--color=never", "check"};
            if (!target.empty()) {
                arguments.emplace_back("--target");
                arguments.push_back(target);
            }
            const auto result = System::RunCaptured(compiler, arguments);
            REQUIRE(result.has_value());
            INFO("Packages/", package, "/README.md, block ", index + 1, " read as a ", Describe(kind), "\n", source,
                 "\n", result->output);

            if (std::ranges::contains(Fragments, name)) {
                seenFragments.push_back(name);
                CHECK_MESSAGE(result->exitCode != 0, name,
                              " compiles on its own, so it is an example rather than prose context. Remove it from "
                              "Fragments so that it is checked as one.");
                continue;
            }
            ++compilable;
            CHECK(result->exitCode == 0);
        }
    }

    // A block count is reported rather than asserted: a new example must not have to be registered anywhere, and a
    // README that loses one must not fail for having done so. The fragment names are the exception, because each
    // one excuses a failure and an entry excusing nothing hides the next one.
    MESSAGE("compiled ", compilable, " blocks; ", seenFragments.size(), " fragments need prose context; ", signatures,
            " illustrate a signature");
    CHECK(compilable > 0);
    std::ranges::sort(seenFragments);
    for (const std::string_view name : Fragments) {
        CHECK_MESSAGE(std::ranges::binary_search(seenFragments, name), name,
                      " names no README block. Remove it from Fragments.");
    }
    std::filesystem::remove_all(root);
}
