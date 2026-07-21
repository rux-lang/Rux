#include "Lexer/Lexer.h"

#include <doctest.h>
#include <string>
#include <utility>

using namespace Rux;

namespace {

LexerResult Lex(std::string source) {
    Lexer lexer(std::move(source), "test.rux");
    return lexer.Tokenize();
}

} // namespace

TEST_CASE("Lexer tokenizes a simple function") {
    const auto result = Lex("func Main() -> int {\n    return 0;\n}\n");
    REQUIRE(result.diagnostics.empty());
    REQUIRE(!result.tokens.empty());
    CHECK(result.tokens.front().Is(TokenKind::FuncKeyword));
    CHECK(result.tokens.back().IsEof());
}

TEST_CASE("Lexer keeps the original source spelling in token text") {
    const auto result = Lex("let x = 0xFF;");
    REQUIRE(result.diagnostics.empty());
    REQUIRE(result.tokens.size() >= 4);
    CHECK(result.tokens[0].Is(TokenKind::LetKeyword));
    CHECK(result.tokens[1].Is(TokenKind::Ident));
    CHECK(result.tokens[3].Is(TokenKind::IntLiteral));
    CHECK(result.tokens[3].text == "0xFF");
}

TEST_CASE("Lexer does not recognize flat compile-time intrinsic aliases") {
    static constexpr const char *aliases[] = {
        "line",
        "column",
        "file",
        "fileName",
        "filePath",
        "function",
        "module",
        "date",
        "time",
        "ruxVersion",
        "os",
        "arch",
        "abi",
        "endian",
        "pointerBits",
        "dataModel",
        "objectFormat",
        "targetTriple",
        "targetFeature",
        "buildProfile",
        "buildMode",
        "optimization",
        "debugAssertions",
        "debugInfo",
        "isTest",
        "outputKind",
        "buildTimestamp",
        "compilerVersion",
        "compilerHasFeature",
        "hasConfig",
    };

    for (const char *alias : aliases) {
        CAPTURE(std::string(alias));
        const auto result = Lex(std::string("#") + alias);
        REQUIRE(result.diagnostics.empty());
        REQUIRE(result.tokens.size() == 3);
        CHECK(result.tokens[0].Is(TokenKind::Hash));
        CHECK(result.tokens[1].text == alias);
    }
}

TEST_CASE("Lexer treats an intrinsic value name as '#' plus an identifier") {
    const auto result = Lex("#target.os");
    REQUIRE(result.diagnostics.empty());
    REQUIRE(result.tokens.size() == 5);
    CHECK(result.tokens[0].Is(TokenKind::Hash));
    CHECK(result.tokens[1].Is(TokenKind::Ident));
    CHECK(result.tokens[1].text == "target");
    CHECK(result.tokens[2].Is(TokenKind::Dot));
    CHECK(result.tokens[3].text == "os");
}

TEST_CASE("Lexer recognizes intrinsic value declarations") {
    const auto result = Lex("intrinsic #target: Target;");
    REQUIRE(result.diagnostics.empty());
    REQUIRE(result.tokens.size() == 7);
    CHECK(result.tokens[0].Is(TokenKind::IntrinsicKeyword));
    CHECK(result.tokens[1].Is(TokenKind::Hash));
    CHECK(result.tokens[2].Is(TokenKind::Ident));
    CHECK(result.tokens[2].text == "target");
}

// '$' carried the old compiler-initialized marker and now has no meaning.
TEST_CASE("Lexer rejects '$'") {
    const auto result = Lex("const $target: Target;");
    CHECK_FALSE(result.diagnostics.empty());
}

TEST_CASE("Lexer accepts every control escape sequence") {
    const auto result = Lex(R"(let s = "\n\t\r\a\b\f\v\0\\\"";)");
    CHECK(result.diagnostics.empty());
}

TEST_CASE("Lexer rejects unknown escape sequences") {
    const auto result = Lex(R"(let s = "\q";)");
    REQUIRE(result.HasErrors());
    CHECK(result.diagnostics.front().message == "unknown escape sequence '\\q'");
}

TEST_CASE("Lexer reports the location of a lexical error") {
    const auto result = Lex("let s = \"\\q\";");
    REQUIRE(result.HasErrors());
    const auto &diag = result.diagnostics.front();
    CHECK(diag.location.line == 1);
    CHECK(diag.location.column == 10);
}

TEST_CASE("DecodeCharLiteralCodePoint decodes plain and escaped characters") {
    CHECK(Lexer::DecodeCharLiteralCodePoint("'A'") == 65u);
    CHECK(Lexer::DecodeCharLiteralCodePoint(R"('\n')") == 10u);
    CHECK(Lexer::DecodeCharLiteralCodePoint(R"('\t')") == 9u);
    CHECK(Lexer::DecodeCharLiteralCodePoint(R"('\a')") == 7u);
    CHECK(Lexer::DecodeCharLiteralCodePoint(R"('\b')") == 8u);
    CHECK(Lexer::DecodeCharLiteralCodePoint(R"('\f')") == 12u);
    CHECK(Lexer::DecodeCharLiteralCodePoint(R"('\v')") == 11u);
    CHECK(!Lexer::DecodeCharLiteralCodePoint("''").has_value());
    CHECK(!Lexer::DecodeCharLiteralCodePoint("no quotes").has_value());
}

TEST_CASE("KeywordKind distinguishes keywords from identifiers") {
    CHECK(KeywordKind("intrinsic") == TokenKind::IntrinsicKeyword);
    CHECK(TokenKindName(TokenKind::IntrinsicKeyword) == "IntrinsicKeyword");
    CHECK(KeywordKind("func") == TokenKind::FuncKeyword);
    CHECK(KeywordKind("while") == TokenKind::WhileKeyword);
    CHECK(KeywordKind("if") == TokenKind::IfKeyword);
    CHECK(KeywordKind("when") == TokenKind::WhenKeyword);
    CHECK(KeywordKind("funcy") == TokenKind::Ident);
    CHECK(KeywordKind("whenever") == TokenKind::Ident);
    CHECK(KeywordKind("") == TokenKind::Ident);
}
