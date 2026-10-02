// The README blocks of the first-party packages, read and written out as scratch packages. See
// PackageExampleTests.cpp for why a block is one of four things and how each is wrapped.

#include "PackageExampleSupport.h"

#include <algorithm>
#include <array>
#include <doctest.h>
#include <fstream>
#include <iterator>
#include <utility>

namespace Rux::Testing::PackageExamples {
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
constexpr auto FragmentNames = std::to_array<std::string_view>({
    "Collections3", // `Vector` and `sink` are the running example the section builds up.
    "Collections4", // The same, continued.
    "FileSystem1",  // `WriteAll(file, contents)` shown against the file the prose has just opened.
    "Json1",        // A `match` whose parse and handlers the text describes.
    "Toml1",        // The same shape, over a document the text names.
});

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

std::span<const std::string_view> Fragments() {
    return FragmentNames;
}

std::vector<ReadmeExample> MaterializeReadmeExamples(const std::filesystem::path &root, std::size_t &signatures) {
    std::filesystem::remove_all(root);
    std::error_code created;
    std::filesystem::create_directories(root / "Bin", created);
    REQUIRE(!created);

    std::vector<ReadmeExample> examples;
    signatures = 0;
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
            examples.push_back({.package = package,
                                .block = index + 1,
                                .name = name,
                                .kind = kind,
                                .source = std::move(source),
                                .manifest = scratch / "Rux.toml",
                                .target = TargetFor(package),
                                .fragment = std::ranges::contains(FragmentNames, std::string_view(name))});
        }
    }
    return examples;
}

} // namespace Rux::Testing::PackageExamples
