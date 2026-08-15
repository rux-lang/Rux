#include "Documentation/Generator.h"
#include "Lexer/Lexer.h"
#include "Package/Manifest.h"
#include "Syntax/Parser/Parser.h"
#include "System/Os.h"

#include <array>
#include <doctest.h>
#include <filesystem>
#include <fstream>
#include <string>

using namespace Rux;

namespace {
ParseResult Parse(std::string source, const std::string &name = "Src/Api.rux") {
    Lexer lexer(std::move(source), name);
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), name);
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    return parsed;
}

std::string Read(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
} // namespace

TEST_CASE("outer documentation comments attach without a blank line") {
    auto parsed = Parse("/// First line.\n/// Second line.\npub struct Thing {\n"
                        "    /// A visible field.\n    pub value: int;\n}\n");
    REQUIRE(parsed.module.items.size() == 1);
    const auto *structure = dynamic_cast<const StructDecl *>(parsed.module.items.front().get());
    REQUIRE(structure != nullptr);
    CHECK(structure->documentation == "First line.\nSecond line.");
    REQUIRE(structure->fields.size() == 1);
    CHECK(structure->fields.front().documentation == "A visible field.");
}

TEST_CASE("a blank line prevents documentation attachment") {
    auto parsed = Parse("/// Detached.\n\npub func Visible();\n");
    REQUIRE(parsed.module.items.size() == 1);
    CHECK(parsed.module.items.front()->documentation.empty());
}

TEST_CASE("documentation generator escapes content and protects unmanaged directories") {
    auto parsed = Parse("/// Safe <b>text</b> and [bad](javascript:alert(1)).\n"
                        "pub struct Public {\n    /// `value` field.\n    pub value: int;\n}\n"
                        "/// Hidden.\nfunc Private();\n");
    auto loaded = Manifest::Parse("[Manifest]\nVersion = 1\n\n[Package]\nName = \"DocsTest\"\n"
                                  "Version = \"0.4.0\"\nType = \"SourceLibrary\"\nDescription = \"Docs test\"\n",
                                  "Rux.toml");
    REQUIRE(loaded.Ok());

    const auto root = System::TempDirectory() / "rux-documentation-unit";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    REQUIRE_FALSE(ec);
    const auto output = root / "site";
    Diagnostic error;
    const std::array modules{std::move(parsed)};
    REQUIRE(
        Documentation::Generate(*loaded.manifest, modules, {.packageRoot = root, .outputDirectory = output}, error));
    const auto firstHtml = Read(output / "index.html");
    const auto firstSearch = Read(output / "search-index.json");
    CHECK(firstHtml.contains("Safe &lt;b&gt;text&lt;/b&gt;"));
    CHECK_FALSE(firstHtml.contains("javascript:"));
    CHECK(firstHtml.contains("Public"));
    CHECK_FALSE(firstHtml.contains("Private"));
    CHECK(std::filesystem::exists(output / ".rux-docs"));

    REQUIRE(
        Documentation::Generate(*loaded.manifest, modules, {.packageRoot = root, .outputDirectory = output}, error));
    CHECK(Read(output / "index.html") == firstHtml);
    CHECK(Read(output / "search-index.json") == firstSearch);

    const auto unmanaged = root / "unmanaged";
    std::filesystem::create_directories(unmanaged, ec);
    std::ofstream(unmanaged / "keep.txt") << "keep";
    CHECK_FALSE(
        Documentation::Generate(*loaded.manifest, modules, {.packageRoot = root, .outputDirectory = unmanaged}, error));
    CHECK(error.message.contains("unmarked"));
    CHECK(error.help == "choose an empty output directory or remove its contents");
    CHECK(Read(unmanaged / "keep.txt") == "keep");
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("documentation generator reports destination paths and filesystem details") {
    auto parsed = Parse("pub func Visible();\n");
    auto loaded = Manifest::Parse("[Manifest]\nVersion = 1\n\n[Package]\nName = \"DocsFailure\"\n"
                                  "Version = \"0.4.0\"\nType = \"SourceLibrary\"\n",
                                  "Rux.toml");
    REQUIRE(loaded.Ok());

    const auto root = System::TempDirectory() / "rux-documentation-failure-unit";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    REQUIRE_FALSE(ec);
    const auto blocked = root / "blocked";
    std::ofstream(blocked) << "not a directory\n";

    Diagnostic error;
    const std::array modules{std::move(parsed)};
    CHECK_FALSE(Documentation::Generate(*loaded.manifest, modules,
                                        {.packageRoot = root, .outputDirectory = blocked / "site"}, error));
    CHECK(error.message.contains("could not create temporary documentation directory"));
    CHECK(error.message.contains(blocked.string()));
    REQUIRE(error.notes.size() == 1);
    CHECK(error.notes.front().starts_with("filesystem error "));
    CHECK(error.help == "check that the output directory's parent is writable");
    std::filesystem::remove_all(root, ec);
}
