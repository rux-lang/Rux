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

ManifestResult Docs(const std::string &name) {
    return Manifest::Parse("[Manifest]\nVersion = 1\n\n[Package]\nName = \"" + name +
                               "\"\nVersion = \"0.4.0\"\nType = \"SourceLibrary\"\nDescription = \"Docs test\"\n",
                           "Rux.toml");
}

std::filesystem::path FreshRoot(const std::string &name) {
    const auto root = System::TempDirectory() / name;
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    REQUIRE_FALSE(ec);
    return root;
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

TEST_CASE("documentation covers only effectively public declarations and members") {
    auto parsed = Parse("/// Safe <b>text</b>.\npub struct Plain {\n    /// A field.\n    pub value: int;\n"
                        "    /// Hidden field.\n    hidden: int;\n}\n"
                        "/// Not public.\nfunc Bare();\n"
                        "pub module Inner {\n    /// Exported.\n    pub func Shown();\n"
                        "    /// Kept in.\n    func Withheld();\n}\n"
                        "module Internal {\n    /// Capped by its module.\n    pub func Marked();\n}\n"
                        "struct Secret {}\nextend Secret {\n    /// Capped by its type.\n    pub func Reveal();\n}\n");
    auto loaded = Docs("DocsTest");
    REQUIRE(loaded.Ok());

    const auto root = FreshRoot("rux-documentation-unit");
    const auto output = root / "site";
    const std::array modules{std::move(parsed)};
    auto generated =
        Documentation::Generate(*loaded.manifest, modules, {.packageRoot = root, .outputDirectory = output});
    REQUIRE(generated.ok);
    CHECK(generated.diagnostics.empty());
    const auto firstHtml = Read(output / "index.html");
    const auto firstSearch = Read(output / "search-index.json");
    CHECK(firstHtml.contains("Safe &lt;b&gt;text&lt;/b&gt;"));
    CHECK(firstHtml.contains("Plain"));
    CHECK_FALSE(firstHtml.contains("Bare"));
    CHECK(firstHtml.contains("A field."));
    CHECK_FALSE(firstHtml.contains("Hidden field."));
    CHECK(firstHtml.contains("Shown"));
    CHECK_FALSE(firstHtml.contains("Withheld"));
    CHECK_FALSE(firstHtml.contains("Marked"));
    CHECK_FALSE(firstHtml.contains("Reveal"));
    CHECK(std::filesystem::exists(output / ".rux-docs"));

    // Generating twice over the same directory produces the same bytes.
    auto again = Documentation::Generate(*loaded.manifest, modules, {.packageRoot = root, .outputDirectory = output});
    REQUIRE(again.ok);
    CHECK(Read(output / "index.html") == firstHtml);
    CHECK(Read(output / "search-index.json") == firstSearch);

    const auto unmanaged = root / "unmanaged";
    std::error_code ec;
    std::filesystem::create_directories(unmanaged, ec);
    std::ofstream(unmanaged / "keep.txt") << "keep";
    auto refused =
        Documentation::Generate(*loaded.manifest, modules, {.packageRoot = root, .outputDirectory = unmanaged});
    CHECK_FALSE(refused.ok);
    REQUIRE(refused.diagnostics.size() == 1);
    CHECK(refused.diagnostics.front().message.contains("unmarked"));
    CHECK(refused.diagnostics.front().help == "choose an empty output directory or remove its contents");
    CHECK(Read(unmanaged / "keep.txt") == "keep");
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("documentation renders reference and lifecycle signatures as source syntax") {
    auto parsed = Parse(R"(
        struct Cell { value: int32; }
        extend Cell {
            /// Copy is intentionally unavailable.
            func =(self: &var Cell, other: &Cell);
            /// Move a cell.
            func <-(self: &var Cell, other: Cell) {}
            /// Destroy a cell.
            func ~Cell(self: &var Cell) {}
        }
        /// Observe borrowed cells.
        func Observe(shared: &Cell, exclusive: &var Cell);
    )");
    auto loaded = Docs("LifecycleDocs");
    REQUIRE(loaded.Ok());

    const auto root = FreshRoot("rux-documentation-lifecycle-unit");
    const std::array modules{std::move(parsed)};
    const auto generated = Documentation::Generate(
        *loaded.manifest, modules, {.packageRoot = root, .outputDirectory = root / "site", .includePrivate = true});
    REQUIRE(generated.ok);
    const std::string html = Read(root / "site" / "index.html");
    CHECK(html.contains("func =(self: &amp;var Cell, other: &amp;Cell)"));
    CHECK(html.contains("func &lt;-(self: &amp;var Cell, other: Cell)"));
    CHECK(html.contains("func ~Cell(self: &amp;var Cell)"));
    CHECK(html.contains("func Observe(shared: &amp;Cell, exclusive: &amp;var Cell)"));

    std::error_code error;
    std::filesystem::remove_all(root, error);
}

TEST_CASE("documentation renders public extern block members with complete signatures") {
    auto parsed = Parse(R"(
        #Link("system.dll")
        extern {
            pub func Open(value: int, ...) -> int;
            func Hidden();
            pub Shared: uint64;
        }
    )");
    auto loaded = Docs("ExternDocs");
    REQUIRE(loaded.Ok());

    const auto root = FreshRoot("rux-documentation-extern-unit");
    const std::array modules{std::move(parsed)};
    const auto generated =
        Documentation::Generate(*loaded.manifest, modules, {.packageRoot = root, .outputDirectory = root / "site"});
    REQUIRE(generated.ok);
    const std::string html = Read(root / "site" / "index.html");
    CHECK(html.contains("pub extern func Open(value: int, ...) -&gt; int"));
    CHECK(html.contains("pub extern Shared: uint64"));
    CHECK_FALSE(html.contains("Hidden"));

    std::error_code error;
    std::filesystem::remove_all(root, error);
}

TEST_CASE("documentation refuses a link it cannot emit rather than dropping it quietly") {
    auto parsed = Parse("/// See [bad](javascript:alert(1)).\npub func Thing();\n");
    auto loaded = Docs("DocsLinks");
    REQUIRE(loaded.Ok());

    const auto root = FreshRoot("rux-documentation-link-unit");
    const std::array modules{std::move(parsed)};
    auto generated =
        Documentation::Generate(*loaded.manifest, modules, {.packageRoot = root, .outputDirectory = root / "site"});
    CHECK_FALSE(generated.ok);
    REQUIRE(generated.diagnostics.size() == 1);
    CHECK(generated.diagnostics.front().message.contains("javascript:"));
    CHECK(generated.diagnostics.front().sourceName == "Src/Api.rux");
    // Nothing is installed, so a refused run leaves no half-written site behind.
    std::error_code ec;
    CHECK_FALSE(std::filesystem::exists(root / "site", ec));
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("documentation refuses two declarations that claim one route") {
    // The anchor is built from the name folded to lower case, so two declarations differing only in case land on
    // the same route and every link to the second would reach the first.
    auto parsed = Parse("/// One.\npub func Thing();\n/// Two.\npub struct thing {\n    pub value: int;\n}\n");
    auto loaded = Docs("DocsRoutes");
    REQUIRE(loaded.Ok());

    const auto root = FreshRoot("rux-documentation-route-unit");
    const std::array modules{std::move(parsed)};
    auto generated =
        Documentation::Generate(*loaded.manifest, modules, {.packageRoot = root, .outputDirectory = root / "site"});
    CHECK_FALSE(generated.ok);
    REQUIRE(generated.diagnostics.size() == 1);
    CHECK(generated.diagnostics.front().message.contains("both claim the documentation route"));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("documentation generator reports destination paths and filesystem details") {
    auto parsed = Parse("pub func Visible();\n");
    auto loaded = Manifest::Parse("[Manifest]\nVersion = 1\n\n[Package]\nName = \"DocsFailure\"\n"
                                  "Version = \"0.4.0\"\nType = \"SourceLibrary\"\n",
                                  "Rux.toml");
    REQUIRE(loaded.Ok());

    const auto root = FreshRoot("rux-documentation-failure-unit");
    const auto blocked = root / "blocked";
    std::ofstream(blocked) << "not a directory\n";

    const std::array modules{std::move(parsed)};
    auto generated =
        Documentation::Generate(*loaded.manifest, modules, {.packageRoot = root, .outputDirectory = blocked / "site"});
    CHECK_FALSE(generated.ok);
    REQUIRE(generated.diagnostics.size() == 1);
    const auto &error = generated.diagnostics.front();
    CHECK(error.message.contains("could not create temporary documentation directory"));
    CHECK(error.message.contains(blocked.string()));
    REQUIRE(error.notes.size() == 1);
    CHECK(error.notes.front().starts_with("filesystem error "));
    CHECK(error.help == "check that the output directory's parent is writable");
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}
