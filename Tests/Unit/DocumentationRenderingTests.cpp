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
ParseResult ParseDocumentation(std::string source, const std::string &name = "Src/Structured.rux") {
    auto lexed = Lexer(std::move(source), name).Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    auto parsed = Parser(std::move(lexed.tokens), name).Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    return parsed;
}

Manifest DocumentationManifest() {
    auto parsed = Manifest::Parse("[Manifest]\nVersion = 1\n\n[Package]\nName = \"StructuredDocs\"\n"
                                  "Version = \"1.0.0\"\nType = \"SourceLibrary\"\n"
                                  "Description = \"Structured documentation tests\"\n",
                                  "Rux.toml");
    REQUIRE(parsed.Ok());
    return std::move(*parsed.manifest);
}

std::filesystem::path RenderingRoot(const std::string &name) {
    const auto root = System::TempDirectory() / name;
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    REQUIRE_FALSE(error);
    return root;
}

std::string ReadRenderingFile(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

Documentation::GenerateResult GenerateDocumentationSite(ParseResult parsed, const std::filesystem::path &root,
                                                        const bool includePrivate = false) {
    const std::array modules{std::move(parsed)};
    return Documentation::Generate(
        DocumentationManifest(), modules,
        {.packageRoot = root, .outputDirectory = root / "site", .includePrivate = includePrivate});
}

void RemoveRenderingRoot(const std::filesystem::path &root) {
    std::error_code error;
    std::filesystem::remove_all(root, error);
}
} // namespace

TEST_CASE("documentation renders Markdown headings at item-safe levels") {
    auto parsed = ParseDocumentation(R"(
/// Parses a value.
///
/// # Safety
/// Call only with validated input.
///
/// ## Failures
/// Returns an error for malformed text.
///
/// ### Panics
/// Never.
pub func Parse();
)");
    const auto root = RenderingRoot("rux-structured-doc-headings");
    const auto generated = GenerateDocumentationSite(std::move(parsed), root);
    REQUIRE(generated.ok);
    CHECK(generated.diagnostics.empty());

    const std::string html = ReadRenderingFile(root / "site" / "index.html");
    CHECK(html.contains("<h4 class=\"doc-heading\">Safety</h4>"));
    CHECK(html.contains("<h5 class=\"doc-heading\">Failures</h5>"));
    CHECK(html.contains("<h6 class=\"doc-heading\">Panics</h6>"));
    CHECK(html.contains("<p>Call only with validated input.</p>"));
    CHECK(html.contains("<p>Returns an error for malformed text.</p>"));
    RemoveRenderingRoot(root);
}

TEST_CASE("documentation renders every structured tag section") {
    auto parsed = ParseDocumentation(R"(
/// Parses a value with <unsafe> input.
///
/// @deprecated Use `ParseNew` instead.
/// @typeParam T Produced **value** type.
/// @typeParam E Failure type.
/// @param input Source *text*.
/// @param limit Maximum count.
/// @returns A `T` value.
/// @see Core::Result Related result.
/// @see `ParseNew` Replacement function.
pub func Parse<T, E>(input: String, limit: int) -> T;
)");
    const auto root = RenderingRoot("rux-structured-doc-sections");
    const auto generated = GenerateDocumentationSite(std::move(parsed), root);
    REQUIRE(generated.ok);

    const std::string html = ReadRenderingFile(root / "site" / "index.html");
    CHECK(html.contains("Parses a value with &lt;unsafe&gt; input."));
    CHECK(html.contains("<aside class=\"deprecated\"><h4>Deprecated</h4>"));
    CHECK(html.contains("<section class=\"doc-section type-parameters\"><h4>Type Parameters</h4><dl>"));
    CHECK(html.contains("<dt><code>T</code></dt><dd><p>Produced <strong>value</strong> type.</p></dd>"));
    CHECK(html.contains("<dt><code>E</code></dt><dd><p>Failure type.</p></dd>"));
    CHECK(html.contains("<section class=\"doc-section parameters\"><h4>Parameters</h4><dl>"));
    CHECK(html.contains("<dt><code>input</code></dt><dd><p>Source <em>text</em>.</p></dd>"));
    CHECK(html.contains("<section class=\"doc-section returns\"><h4>Returns</h4><p>A <code>T</code> value.</p>"));
    CHECK(html.contains("<section class=\"doc-section see-also\"><h4>See Also</h4><ul>"));
    CHECK(html.contains("<li><code>Core::Result</code><p>Related result.</p></li>"));
    CHECK(html.contains("<li><code>ParseNew</code><p>Replacement function.</p></li>"));
    CHECK_FALSE(html.contains("href=\"Core::Result\""));
    RemoveRenderingRoot(root);
}

TEST_CASE("structured section groups follow the first authored tag occurrence") {
    auto parsed = ParseDocumentation(R"(
/// Ordered sections.
/// @see Core::Item Related item.
/// @param first First value.
/// @deprecated Use Next.
/// @param second Second value.
/// @returns The result.
pub func Ordered(first: int, second: int) -> int;
)");
    const auto root = RenderingRoot("rux-structured-doc-order");
    const auto generated = GenerateDocumentationSite(std::move(parsed), root);
    REQUIRE(generated.ok);
    const std::string html = ReadRenderingFile(root / "site" / "index.html");

    const auto see = html.find("<h4>See Also</h4>");
    const auto parameters = html.find("<h4>Parameters</h4>");
    const auto deprecated = html.find("<h4>Deprecated</h4>");
    const auto returns = html.find("<h4>Returns</h4>");
    REQUIRE(see != std::string::npos);
    REQUIRE(parameters != std::string::npos);
    REQUIRE(deprecated != std::string::npos);
    REQUIRE(returns != std::string::npos);
    CHECK(see < parameters);
    CHECK(parameters < deprecated);
    CHECK(deprecated < returns);
    CHECK(html.find("<code>first</code>") < html.find("<code>second</code>"));
    RemoveRenderingRoot(root);
}

TEST_CASE("Markdown fences keep heading and tag-looking text literal") {
    auto parsed = ParseDocumentation(R"(
/// Example.
///
/// ~~~text
/// # Not a heading
/// @param not_a_tag remains prose
/// <script>alert(1)</script>
/// ~~~
pub struct Example {}
)");
    const auto root = RenderingRoot("rux-structured-doc-fence");
    const auto generated = GenerateDocumentationSite(std::move(parsed), root);
    REQUIRE(generated.ok);
    const std::string html = ReadRenderingFile(root / "site" / "index.html");
    CHECK(html.contains("<pre><code># Not a heading\n@param not_a_tag remains prose\n"));
    CHECK(html.contains("&lt;script&gt;alert(1)&lt;/script&gt;"));
    CHECK_FALSE(html.contains("class=\"doc-heading\">Not a heading"));
    CHECK_FALSE(html.contains("<h4>Parameters</h4>"));
    RemoveRenderingRoot(root);
}

TEST_CASE("invalid documentation on an included declaration fails generation") {
    auto parsed = ParseDocumentation(R"(
/// Broken docs.
/// @return Unknown spelling.
pub func Broken();
)");
    const auto root = RenderingRoot("rux-structured-doc-invalid-public");
    const auto generated = GenerateDocumentationSite(std::move(parsed), root);
    CHECK_FALSE(generated.ok);
    REQUIRE_EQ(generated.diagnostics.size(), 1);
    const auto &error = generated.diagnostics.front();
    CHECK(error.message.contains("invalid documentation"));
    CHECK(error.message.contains("unknown documentation tag '@return'"));
    CHECK_EQ(error.sourceName, "Src/Structured.rux");
    CHECK_EQ(error.location.line, 3);
    CHECK_FALSE(std::filesystem::exists(root / "site"));
    RemoveRenderingRoot(root);
}

TEST_CASE("invalid documentation on filtered private items is ignored") {
    constexpr std::string_view source = R"(
/// Public item.
pub func Public();

/// Private malformed item.
/// @param
func Private();
)";
    const auto root = RenderingRoot("rux-structured-doc-invalid-private");
    auto generated = GenerateDocumentationSite(ParseDocumentation(std::string(source)), root);
    REQUIRE(generated.ok);
    CHECK(generated.diagnostics.empty());
    CHECK_FALSE(ReadRenderingFile(root / "site" / "index.html").contains("Private"));

    std::error_code error;
    std::filesystem::remove_all(root / "site", error);
    generated = GenerateDocumentationSite(ParseDocumentation(std::string(source)), root, true);
    CHECK_FALSE(generated.ok);
    REQUIRE_EQ(generated.diagnostics.size(), 1);
    CHECK(generated.diagnostics.front().message.contains("@param requires"));
    RemoveRenderingRoot(root);
}

TEST_CASE("invalid documentation is checked only on rendered members") {
    auto parsed = ParseDocumentation(R"(
/// A record.
pub struct Record {
    /// @unknown Visible field issue.
    pub visible: int;
    /// @unknown Hidden field issue.
    hidden: int;
}
)");
    const auto root = RenderingRoot("rux-structured-doc-invalid-member");
    const auto generated = GenerateDocumentationSite(std::move(parsed), root);
    CHECK_FALSE(generated.ok);
    REQUIRE_EQ(generated.diagnostics.size(), 1);
    CHECK(generated.diagnostics.front().message.contains("Visible field issue") == false);
    CHECK(generated.diagnostics.front().message.contains("unknown documentation tag '@unknown'"));
    CHECK_EQ(generated.diagnostics.front().location.line, 4);
    RemoveRenderingRoot(root);
}

TEST_CASE("structured documentation output is deterministic") {
    const std::string source = R"(
/// Computes a value.
/// @param value Input value.
/// @returns Output value.
pub func Compute(value: int) -> int;
)";
    const auto root = RenderingRoot("rux-structured-doc-deterministic");
    auto first = GenerateDocumentationSite(ParseDocumentation(source), root);
    REQUIRE(first.ok);
    const std::string firstHtml = ReadRenderingFile(root / "site" / "index.html");
    const std::string firstSearch = ReadRenderingFile(root / "site" / "search-index.json");

    auto second = GenerateDocumentationSite(ParseDocumentation(source), root);
    REQUIRE(second.ok);
    CHECK_EQ(ReadRenderingFile(root / "site" / "index.html"), firstHtml);
    CHECK_EQ(ReadRenderingFile(root / "site" / "search-index.json"), firstSearch);
    RemoveRenderingRoot(root);
}

TEST_CASE("structured sections render on interface and extension methods") {
    auto parsed = ParseDocumentation(R"(
/// A readable value.
pub interface Reader {
    /// Reads a value.
    /// @param count Requested count.
    /// @returns The value read.
    func Read(count: int) -> int;
}

/// A stored value.
pub struct Store {}
extend Store {
    /// Writes a value.
    /// @param value Value to write.
    /// @see Reader::Read Related read operation.
    pub func Write(value: int);
}
)");
    const auto root = RenderingRoot("rux-structured-doc-methods");
    const auto generated = GenerateDocumentationSite(std::move(parsed), root);
    REQUIRE(generated.ok);
    CHECK(generated.diagnostics.empty());
    const std::string html = ReadRenderingFile(root / "site" / "index.html");

    const auto reader = html.find("<h3>Reader</h3>");
    const auto read = html.find("<h4>Read</h4>", reader);
    const auto readParameters = html.find("<h4>Parameters</h4>", read);
    const auto readReturns = html.find("<h4>Returns</h4>", read);
    REQUIRE(reader != std::string::npos);
    REQUIRE(read != std::string::npos);
    REQUIRE(readParameters != std::string::npos);
    REQUIRE(readReturns != std::string::npos);
    CHECK(read < readParameters);
    CHECK(readParameters < readReturns);

    const auto write = html.find("<h4>Write</h4>");
    const auto writeParameters = html.find("<h4>Parameters</h4>", write);
    const auto writeSee = html.find("<h4>See Also</h4>", write);
    REQUIRE(write != std::string::npos);
    REQUIRE(writeParameters != std::string::npos);
    REQUIRE(writeSee != std::string::npos);
    CHECK(write < writeParameters);
    CHECK(writeParameters < writeSee);
    CHECK(html.contains("<code>Reader::Read</code>"));
    RemoveRenderingRoot(root);
}

TEST_CASE("all included documentation issues are reported in source order") {
    auto parsed = ParseDocumentation(R"(
/// First broken item.
/// @unknown Unknown tag.
/// @returns One.
/// @returns Duplicate.
pub func First() -> int;

/// Second broken item.
/// @see javascript:run Unsafe target.
pub struct Second {}
)");
    const auto root = RenderingRoot("rux-structured-doc-issue-order");
    const auto generated = GenerateDocumentationSite(std::move(parsed), root);
    CHECK_FALSE(generated.ok);
    REQUIRE_EQ(generated.diagnostics.size(), 3);
    CHECK(generated.diagnostics[0].message.contains("unknown documentation tag '@unknown'"));
    CHECK(generated.diagnostics[1].message.contains("duplicate documentation tag '@returns'"));
    CHECK(generated.diagnostics[2].message.contains("unsafe or unsupported scheme"));
    CHECK_EQ(generated.diagnostics[0].location.line, 3);
    CHECK_EQ(generated.diagnostics[1].location.line, 5);
    CHECK_EQ(generated.diagnostics[2].location.line, 9);
    for (const auto &diagnostic : generated.diagnostics) {
        CHECK(diagnostic.severity == Diagnostic::Severity::Error);
        CHECK_EQ(diagnostic.sourceName, "Src/Structured.rux");
        REQUIRE(diagnostic.help.has_value());
        CHECK(diagnostic.help->contains("fix the documentation comment"));
    }
    CHECK_FALSE(std::filesystem::exists(root / "site"));
    RemoveRenderingRoot(root);
}

TEST_CASE("structured tag Markdown uses the same safe inline renderer as prose") {
    auto parsed = ParseDocumentation(R"(
/// Downloads a value.
/// @deprecated Read the [migration guide](https://example.com/migrate).
/// @returns **Escaped** `<value>`.
/// @see https://example.com/reference External reference.
pub func Download() -> String;
)");
    const auto root = RenderingRoot("rux-structured-doc-inline-markdown");
    const auto generated = GenerateDocumentationSite(std::move(parsed), root);
    REQUIRE(generated.ok);
    const std::string html = ReadRenderingFile(root / "site" / "index.html");
    CHECK(html.contains("<a href=\"https://example.com/migrate\">migration guide</a>"));
    CHECK(html.contains("<strong>Escaped</strong> <code>&lt;value&gt;</code>"));
    CHECK(html.contains("<li><code>https://example.com/reference</code><p>External reference.</p></li>"));
    CHECK_FALSE(html.contains("<a href=\"https://example.com/reference\""));
    RemoveRenderingRoot(root);
}
