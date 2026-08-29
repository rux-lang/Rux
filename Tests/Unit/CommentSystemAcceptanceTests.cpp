#include "Formatter/Formatter.h"
#include "Lexer/Lexer.h"
#include "Linter/Linter.h"
#include "Syntax/Ast/Ast.h"
#include "Syntax/Parser/Parser.h"

#include <algorithm>
#include <doctest.h>
#include <filesystem>
#include <fstream>
#include <string>

using namespace Rux;

namespace {
std::string CommentFixture() {
    const std::filesystem::path path =
        std::filesystem::path(RUX_TESTS_DIR) / "Language" / "Comments" / "Src" / "Main.rux";
    std::ifstream input(path, std::ios::binary);
    REQUIRE_MESSAGE(input.good(), "comment language fixture not found: ", path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

const Decl *FindDeclaration(const Module &module, const std::string_view name) {
    for (const auto &item : module.items) {
        if (const auto *function = dynamic_cast<const FuncDecl *>(item.get());
            function != nullptr && function->name == name) {
            return function;
        }
        if (const auto *structure = dynamic_cast<const StructDecl *>(item.get());
            structure != nullptr && structure->name == name) {
            return structure;
        }
    }
    return nullptr;
}
} // namespace

TEST_CASE("comment language fixture crosses lexer parser formatter and linter") {
    const std::string source = CommentFixture();
    auto lexed = Lexer(source, "Tests/Language/Comments/Src/Main.rux").Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    REQUIRE(lexed.comments.size() > 30);

    std::size_t ordinaryLines = 0;
    std::size_t ordinaryBlocks = 0;
    std::size_t documentationLines = 0;
    std::size_t documentationBlocks = 0;
    for (const auto &comment : lexed.comments) {
        CHECK(comment.terminated);
        CHECK_EQ(source.substr(comment.range.start.offset, comment.range.Length()), comment.raw);
        switch (comment.kind) {
        case CommentKind::Line:
            ++ordinaryLines;
            break;
        case CommentKind::Block:
            ++ordinaryBlocks;
            break;
        case CommentKind::DocumentationLine:
            ++documentationLines;
            break;
        case CommentKind::DocumentationBlock:
            ++documentationBlocks;
            break;
        }
    }
    CHECK_EQ(ordinaryLines, 6);
    CHECK_EQ(ordinaryBlocks, 8);
    CHECK(documentationLines >= 20);
    CHECK(documentationBlocks >= 4);

    auto parsed = Parser(std::move(lexed.tokens), "Tests/Language/Comments/Src/Main.rux").Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    const Decl *line = FindDeclaration(parsed.module, "LineDocumented");
    const Decl *block = FindDeclaration(parsed.module, "MultilineBlockDocumented");
    const Decl *mixed = FindDeclaration(parsed.module, "MixedDocumented");
    const Decl *malformed = FindDeclaration(parsed.module, "MalformedDocumentation");
    REQUIRE(line != nullptr);
    REQUIRE(block != nullptr);
    REQUIRE(mixed != nullptr);
    REQUIRE(malformed != nullptr);
    CHECK_EQ(line->documentation.Summary(), "Returns a value documented with line comments.");
    CHECK(block->documentation.markdown.contains("# Failures"));
    CHECK(mixed->documentation.markdown.contains("first half"));
    CHECK(mixed->documentation.markdown.contains("second half"));
    CHECK_EQ(malformed->documentation.issues.size(), 2);
    CHECK_EQ(parsed.module.documentationIssues.size(), 2);

    const auto formatted = Formatting::Format(source);
    const auto repeated = Formatting::Format(formatted.text);
    CHECK_FALSE(repeated.changed);
    CHECK_EQ(repeated.text, formatted.text);
    CHECK(formatted.text.contains("//// A fourth slash stays an ordinary decorative comment."));
    CHECK(formatted.text.contains("/***/ // Three opening stars are decorative ordinary text."));
    CHECK(formatted.text.contains("\"/// text inside a string\""));

    const auto linted = Linting::Lint(source, "Tests/Language/Comments/Src/Main.rux");
    CHECK_FALSE(linted.HasErrors());
    CHECK(std::ranges::any_of(linted.diagnostics, [](const Diagnostic &diagnostic) {
        return diagnostic.message.contains("@param requires");
    }));
    CHECK(std::ranges::any_of(linted.diagnostics, [](const Diagnostic &diagnostic) {
        return diagnostic.message.contains("unknown documentation tag '@unknown'");
    }));
    CHECK(std::ranges::count_if(linted.diagnostics, [](const Diagnostic &diagnostic) {
              return diagnostic.message.contains("documentation comment is not attached");
          }) >= 2);
}

TEST_CASE("unterminated documentation remains lossless for diagnostics and formatting") {
    constexpr std::string_view source = "/** open\n * still open";
    const auto lexed = Lexer(std::string(source), "unterminated-doc.rux").Tokenize();
    REQUIRE(lexed.HasErrors());
    REQUIRE_EQ(lexed.comments.size(), 1);
    CHECK(lexed.comments.front().kind == CommentKind::DocumentationBlock);
    CHECK_FALSE(lexed.comments.front().terminated);
    CHECK_EQ(lexed.comments.front().raw, source);
    CHECK(lexed.comments.front().range.start == SourceLocation{1, 1, 0});
    CHECK_EQ(lexed.comments.front().range.end.offset, source.size());

    const auto formatted = Formatting::Format(source);
    CHECK_EQ(formatted.text, std::string(source) + "\n");
    CHECK(Formatting::Format(formatted.text).text == formatted.text);
}
