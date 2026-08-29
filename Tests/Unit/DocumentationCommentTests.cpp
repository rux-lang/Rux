#include "Lexer/Lexer.h"
#include "Syntax/Ast/Ast.h"
#include "Syntax/DocumentationComment.h"
#include "Syntax/Parser/Parser.h"

#include <doctest.h>
#include <string>
#include <string_view>
#include <utility>

using namespace Rux;

namespace {
std::string ParseStructureDocumentation(std::string source) {
    Lexer lexer(std::move(source), "documentation-comment.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE(lexed.diagnostics.empty());

    Parser parser(std::move(lexed.tokens), "documentation-comment.rux");
    auto parsed = parser.Parse();
    REQUIRE(parsed.diagnostics.empty());
    REQUIRE_EQ(parsed.module.items.size(), 1);

    const auto *structure = dynamic_cast<const StructDecl *>(parsed.module.items.front().get());
    REQUIRE(structure != nullptr);
    return structure->documentation;
}
} // namespace

TEST_CASE("Line documentation removes its marker and one optional space") {
    struct Case {
        std::string_view raw;
        std::string_view expected;
    };

    static constexpr Case cases[] = {
        {"/// Summary.", "Summary."},
        {"///Summary.", "Summary."},
        {"///  Indented once.", " Indented once."},
        {"///\tTabbed.", "\tTabbed."},
        {"///", ""},
        {"/// ", ""},
        {"///  ", " "},
    };

    for (const auto &[raw, expected] : cases) {
        CAPTURE(raw);
        CHECK_EQ(NormalizeDocumentationComment(raw), expected);
    }
}

TEST_CASE("One-line block documentation removes only delimiter padding") {
    struct Case {
        std::string_view raw;
        std::string_view expected;
    };

    static constexpr Case cases[] = {
        {"/** Summary. */", "Summary."},
        {"/**Summary.*/", "Summary."},
        {"/**  Deliberately padded.  */", " Deliberately padded. "},
        {"/** */", ""},
        {"/**  */", ""},
        {"/**\tTabbed.\t*/", "\tTabbed.\t"},
        {"/** `code` and **strong**. */", "`code` and **strong**."},
    };

    for (const auto &[raw, expected] : cases) {
        CAPTURE(raw);
        CHECK_EQ(NormalizeDocumentationComment(raw), expected);
    }
}

TEST_CASE("Block documentation normalizes every source line ending to LF") {
    CHECK_EQ(NormalizeDocumentationComment("/**\r\n * First.\r\n * Second.\r\n */"), "First.\nSecond.");
    CHECK_EQ(NormalizeDocumentationComment("/**\r * First.\r * Second.\r */"), "First.\nSecond.");
    CHECK_EQ(NormalizeDocumentationComment("/**\n * First.\r\n * Second.\r */"), "First.\nSecond.");
}

TEST_CASE("Aligned star margins are removed without flattening blank lines") {
    const std::string raw = "/**\n"
                            " * Summary paragraph.\n"
                            " *\n"
                            " * Second paragraph.\n"
                            " */";
    CHECK_EQ(NormalizeDocumentationComment(raw), "Summary paragraph.\n\nSecond paragraph.");
}

TEST_CASE("Common indentation is removed from undecorated block documentation") {
    const std::string raw = "/**\n"
                            "        Summary.\n"
                            "          More-indented detail.\n"
                            "\n"
                            "        End.\n"
                            "    */";
    CHECK_EQ(NormalizeDocumentationComment(raw), "Summary.\n  More-indented detail.\n\nEnd.");
}

TEST_CASE("Markdown indentation survives aligned block decoration") {
    const std::string raw = "/**\n"
                            " * Calls the parser.\n"
                            " *\n"
                            " *     let parsed = Parse(input);\n"
                            " *     return parsed?;\n"
                            " *\n"
                            " * - first\n"
                            " *   - nested\n"
                            " */";
    const std::string expected = "Calls the parser.\n"
                                 "\n"
                                 "    let parsed = Parse(input);\n"
                                 "    return parsed?;\n"
                                 "\n"
                                 "- first\n"
                                 "  - nested";
    CHECK_EQ(NormalizeDocumentationComment(raw), expected);
}

TEST_CASE("Fenced Markdown content and tag-looking text are preserved") {
    const std::string raw = "/**\n"
                            " * Example:\n"
                            " *\n"
                            " * ```rux\n"
                            " * @param is code here\n"
                            " * let text = \"/** literal */\";\n"
                            " * ```\n"
                            " *\n"
                            " * @param input Authored tag text.\n"
                            " */";
    const std::string expected = "Example:\n"
                                 "\n"
                                 "```rux\n"
                                 "@param is code here\n"
                                 "let text = \"/** literal */\";\n"
                                 "```\n"
                                 "\n"
                                 "@param input Authored tag text.";
    CHECK_EQ(NormalizeDocumentationComment(raw), expected);
}

TEST_CASE("Documentation normalization never reflows authored wrapping") {
    const std::string raw = "/**\n"
                            " * A deliberately short\n"
                            " * line followed by a much longer line that remains exactly where the author placed it.\n"
                            " *   two leading spaces remain\n"
                            " */";
    const std::string expected =
        "A deliberately short\n"
        "line followed by a much longer line that remains exactly where the author placed it.\n"
        "  two leading spaces remain";
    CHECK_EQ(NormalizeDocumentationComment(raw), expected);
}

TEST_CASE("A star is decoration only when the nonblank lines align") {
    CHECK_EQ(NormalizeDocumentationComment("/**\n * one\n   two\n */"), "* one\n  two");
    CHECK_EQ(NormalizeDocumentationComment("/**\n **strong**\n **also strong**\n */"), "**strong**\n**also strong**");
    CHECK_EQ(NormalizeDocumentationComment("/**\n * one\n *two is not decorated\n */"), "* one\n*two is not decorated");
    CHECK_EQ(NormalizeDocumentationComment("/**\n * one\n * two\n */"), "one\ntwo");
}

TEST_CASE("Only delimiter boundary lines are discarded") {
    const std::string raw = "/**   \n"
                            "\n"
                            " * Body.\n"
                            "\n"
                            "   */";
    CHECK_EQ(NormalizeDocumentationComment(raw), "\nBody.\n");
}

TEST_CASE("Unrecognized and unterminated spellings survive normalization") {
    struct Case {
        std::string_view raw;
    };

    static constexpr Case cases[] = {
        {"// ordinary"},      {"//// decorative"}, {"/**/"}, {"/*** decorative */"},
        {"/** unterminated"}, {"plain text"},      {""},
    };

    for (const auto &[raw] : cases) {
        CAPTURE(raw);
        CHECK_EQ(NormalizeDocumentationComment(raw), raw);
    }
}

TEST_CASE("Line and block forms normalize to equivalent Markdown") {
    struct Case {
        std::string_view line;
        std::string_view block;
    };

    static constexpr Case cases[] = {
        {"/// Summary.", "/** Summary. */"},
        {"/// `Code`.", "/**\n * `Code`.\n */"},
        {"///     indented", "/**\n *     indented\n */"},
        {"/// @returns A value.", "/**\n * @returns A value.\n */"},
    };

    for (const auto &[line, block] : cases) {
        CAPTURE(line);
        CAPTURE(block);
        CHECK_EQ(NormalizeDocumentationComment(line), NormalizeDocumentationComment(block));
    }
}

TEST_CASE("The parser stores normalized block documentation") {
    CHECK_EQ(ParseStructureDocumentation("/** A documented type. */\n"
                                         "pub struct Item {}\n"),
             "A documented type.");

    CHECK_EQ(ParseStructureDocumentation("/**\n"
                                         " * A documented type.\n"
                                         " *\n"
                                         " * # Safety\n"
                                         " *\n"
                                         " * Callers uphold the invariant.\n"
                                         " */\n"
                                         "pub struct Item {}\n"),
             "A documented type.\n\n# Safety\n\nCallers uphold the invariant.");
}

TEST_CASE("The parser joins mixed adjacent documentation forms") {
    const std::string source = "/// First line.\n"
                               "/** Second line. */\n"
                               "///\n"
                               "/**\n"
                               " * Final paragraph.\n"
                               " */\n"
                               "pub struct Item {}\n";
    CHECK_EQ(ParseStructureDocumentation(source), "First line.\nSecond line.\n\nFinal paragraph.");
}
