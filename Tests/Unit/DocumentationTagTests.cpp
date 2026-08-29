#include "Lexer/Lexer.h"
#include "Syntax/Ast/Ast.h"
#include "Syntax/Parser/Parser.h"

#include <doctest.h>
#include <string>
#include <utility>

using namespace Rux;

namespace {
Syntax::Documentation ParseFunctionDocumentation(std::string source) {
    Lexer lexer(std::move(source), "documentation-tags.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE(lexed.diagnostics.empty());

    Parser parser(std::move(lexed.tokens), "documentation-tags.rux");
    auto parsed = parser.Parse();
    REQUIRE(parsed.diagnostics.empty());
    REQUIRE_EQ(parsed.module.items.size(), 1);
    return parsed.module.items.front()->documentation;
}
} // namespace

TEST_CASE("Documentation parses the five structured tags in authored order") {
    const auto documentation = ParseFunctionDocumentation("/// Parses a value.\n"
                                                          "///\n"
                                                          "/// @deprecated Use ParseStrict.\n"
                                                          "/// @typeParam T The produced value.\n"
                                                          "/// @param input The source text.\n"
                                                          "/// @returns A parsed value.\n"
                                                          "/// @see Core::Result Related result type.\n"
                                                          "func Parse<T>(input: String) -> T;\n");

    CHECK_EQ(documentation.markdown, "Parses a value.");
    REQUIRE_EQ(documentation.tags.size(), 5);
    CHECK(documentation.tags[0].kind == Syntax::DocumentationTagKind::Deprecated);
    CHECK(documentation.tags[1].kind == Syntax::DocumentationTagKind::TypeParameter);
    CHECK(documentation.tags[2].kind == Syntax::DocumentationTagKind::Parameter);
    CHECK(documentation.tags[3].kind == Syntax::DocumentationTagKind::Returns);
    CHECK(documentation.tags[4].kind == Syntax::DocumentationTagKind::See);
    CHECK_EQ(documentation.tags[1].subject, "T");
    CHECK_EQ(documentation.tags[2].subject, "input");
    CHECK_EQ(documentation.tags[4].subject, "Core::Result");
    CHECK_EQ(documentation.tags[4].markdown, "Related result type.");
    CHECK(documentation.issues.empty());
}

TEST_CASE("A direct tag block and a blank-separated block are both accepted") {
    const auto direct = ParseFunctionDocumentation("/// Summary.\n"
                                                   "/// @returns Value.\n"
                                                   "func Direct() -> int;\n");
    const auto separated = ParseFunctionDocumentation("/// Summary.\n"
                                                      "///\n"
                                                      "/// @returns Value.\n"
                                                      "func Separated() -> int;\n");
    CHECK_EQ(direct.markdown, "Summary.");
    CHECK_EQ(separated.markdown, "Summary.");
    REQUIRE_EQ(direct.tags.size(), 1);
    REQUIRE_EQ(separated.tags.size(), 1);
    CHECK_EQ(direct.tags[0].markdown, separated.tags[0].markdown);
    CHECK(direct.issues.empty());
    CHECK(separated.issues.empty());
}

TEST_CASE("Two-space lines continue the preceding tag as Markdown") {
    const auto documentation = ParseFunctionDocumentation("/// Summary.\n"
                                                          "///\n"
                                                          "/// @param input First line.\n"
                                                          "///   Second line with **Markdown**.\n"
                                                          "///\n"
                                                          "///   Final paragraph.\n"
                                                          "func Read(input: String);\n");
    REQUIRE_EQ(documentation.tags.size(), 1);
    CHECK_EQ(documentation.tags[0].markdown, "First line.\nSecond line with **Markdown**.\n\nFinal paragraph.");
    CHECK_EQ(documentation.tags[0].range.start.line, 3);
    CHECK_EQ(documentation.tags[0].range.end.line, 6);
    CHECK(documentation.issues.empty());
}

TEST_CASE("Tag-looking lines inside fenced code remain prose") {
    const auto documentation = ParseFunctionDocumentation("/// Example.\n"
                                                          "///\n"
                                                          "/// ~~~rux\n"
                                                          "/// @param code Is not a tag.\n"
                                                          "/// @returns Also not a tag.\n"
                                                          "/// ~~~\n"
                                                          "///\n"
                                                          "/// @see Core::Parser Actual tag.\n"
                                                          "func Example();\n");
    CHECK(documentation.markdown.contains("@param code Is not a tag."));
    CHECK(documentation.markdown.contains("@returns Also not a tag."));
    REQUIRE_EQ(documentation.tags.size(), 1);
    CHECK(documentation.tags[0].kind == Syntax::DocumentationTagKind::See);
    CHECK_EQ(documentation.tags[0].subject, "Core::Parser");
}

TEST_CASE("Unknown aliases and case variants are recoverable syntax issues") {
    const auto documentation = ParseFunctionDocumentation("/// Summary.\n"
                                                          "/// @return Value.\n"
                                                          "/// @typeparam T Value type.\n"
                                                          "/// @Param input Source.\n"
                                                          "/// @throws Error Never supported.\n"
                                                          "func Read<T>(input: String) -> T;\n");
    CHECK(documentation.tags.empty());
    REQUIRE_EQ(documentation.issues.size(), 4);
    for (const auto &issue : documentation.issues) {
        CHECK(issue.kind == Syntax::DocumentationIssueKind::UnknownTag);
        CHECK(issue.message.contains("unknown documentation tag"));
        CHECK(issue.range.start.line == issue.range.end.line);
    }
}

TEST_CASE("Missing tag subjects and descriptions are diagnosed") {
    const auto documentation = ParseFunctionDocumentation("/// Summary.\n"
                                                          "/// @param\n"
                                                          "/// @param value\n"
                                                          "/// @typeParam 42 Description.\n"
                                                          "/// @returns\n"
                                                          "/// @deprecated\n"
                                                          "/// @see\n"
                                                          "func Broken();\n");
    CHECK(documentation.tags.empty());
    REQUIRE_EQ(documentation.issues.size(), 6);
    for (const auto &issue : documentation.issues) {
        CHECK(issue.kind == Syntax::DocumentationIssueKind::MalformedTag);
        CHECK(issue.range.Length() > 0);
    }
}

TEST_CASE("Unique tags and repeated subjects report duplicates without reordering") {
    const auto documentation = ParseFunctionDocumentation("/// @param value First.\n"
                                                          "/// @param value Second.\n"
                                                          "/// @typeParam T First.\n"
                                                          "/// @typeParam T Second.\n"
                                                          "/// @returns First.\n"
                                                          "/// @returns Second.\n"
                                                          "/// @deprecated First.\n"
                                                          "/// @deprecated Second.\n"
                                                          "/// @see Core::Item First.\n"
                                                          "/// @see Core::Item Second.\n"
                                                          "func Duplicate<T>(value: int) -> int;\n");
    REQUIRE_EQ(documentation.tags.size(), 10);
    REQUIRE_EQ(documentation.issues.size(), 5);
    for (const auto &issue : documentation.issues) {
        CHECK(issue.kind == Syntax::DocumentationIssueKind::DuplicateTag);
    }
    CHECK_EQ(documentation.tags[0].markdown, "First.");
    CHECK_EQ(documentation.tags[1].markdown, "Second.");
}

TEST_CASE("See tags accept web paths and symbolic references") {
    const auto documentation = ParseFunctionDocumentation("/// @see https://rux-lang.dev/docs/api Web documentation.\n"
                                                          "/// @see http://example.test\n"
                                                          "/// @see Core::Result Local type.\n"
                                                          "/// @see `Result<T, E>` Generic result.\n"
                                                          "func Related();\n");
    REQUIRE_EQ(documentation.tags.size(), 4);
    CHECK_EQ(documentation.tags[0].subject, "https://rux-lang.dev/docs/api");
    CHECK_EQ(documentation.tags[1].markdown, "");
    CHECK_EQ(documentation.tags[2].subject, "Core::Result");
    CHECK_EQ(documentation.tags[3].subject, "Result<T, E>");
    CHECK_EQ(documentation.tags[3].markdown, "Generic result.");
    CHECK(documentation.issues.empty());
}

TEST_CASE("Unsafe and malformed See targets are distinguished") {
    const auto documentation = ParseFunctionDocumentation("/// @see javascript:alert(1) Unsafe.\n"
                                                          "/// @see ftp://example.test Unsupported.\n"
                                                          "/// @see bad/path Invalid.\n"
                                                          "/// @see `` Empty.\n"
                                                          "func Related();\n");
    CHECK(documentation.tags.empty());
    REQUIRE_EQ(documentation.issues.size(), 4);
    CHECK(documentation.issues[0].kind == Syntax::DocumentationIssueKind::UnsafeReference);
    CHECK(documentation.issues[1].kind == Syntax::DocumentationIssueKind::UnsafeReference);
    CHECK(documentation.issues[2].kind == Syntax::DocumentationIssueKind::MalformedTag);
    CHECK(documentation.issues[3].kind == Syntax::DocumentationIssueKind::MalformedTag);
}

TEST_CASE("Prose and malformed continuations after tags are retained for recovery") {
    const auto documentation = ParseFunctionDocumentation("/// Summary.\n"
                                                          "/// @returns First line.\n"
                                                          "///  Only one leading space.\n"
                                                          "/// Resumed prose.\n"
                                                          "func Recover() -> int;\n");
    REQUIRE_EQ(documentation.tags.size(), 1);
    CHECK_EQ(documentation.tags[0].markdown, "First line.\n Only one leading space.\nResumed prose.");
    REQUIRE_EQ(documentation.issues.size(), 2);
    CHECK(documentation.issues[0].kind == Syntax::DocumentationIssueKind::MalformedContinuation);
    CHECK(documentation.issues[1].kind == Syntax::DocumentationIssueKind::ProseAfterTags);
}

TEST_CASE("Structured tag ranges point at normalized line content") {
    const auto line = ParseFunctionDocumentation("/// Summary.\r\n"
                                                 "/// @param value Description.\r\n"
                                                 "func Line(value: int);\r\n");
    REQUIRE_EQ(line.tags.size(), 1);
    CHECK(line.tags[0].range.start == SourceLocation{2, 5, 18});
    CHECK_EQ(line.tags[0].range.end.line, 2);
    CHECK(line.tags[0].range.end.offset > line.tags[0].range.start.offset);

    const auto block = ParseFunctionDocumentation("/**\n"
                                                  " * Summary.\n"
                                                  " * @returns Value.\n"
                                                  " */\n"
                                                  "func Block() -> int;\n");
    REQUIRE_EQ(block.tags.size(), 1);
    CHECK(block.tags[0].range.start == SourceLocation{3, 4, 19});
    CHECK_EQ(block.tags[0].range.end.line, 3);
}

TEST_CASE("Escaped and indented at-sign prose does not start a tag block") {
    const auto documentation = ParseFunctionDocumentation("/// \\@param is literal prose.\n"
                                                          "///  @returns is indented prose.\n"
                                                          "func Literal();\n");
    CHECK_EQ(documentation.markdown, "\\@param is literal prose.\n @returns is indented prose.");
    CHECK(documentation.tags.empty());
    CHECK(documentation.issues.empty());
}
