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

TEST_CASE("Lexer returns source-file open failures as diagnostics") {
    const auto result = Lexer::FromFile("rux-lexer-test-file-that-does-not-exist.rux");
    REQUIRE_EQ(result.diagnostics.size(), 1);
    CHECK_EQ(result.diagnostics[0].message, "cannot open source file 'rux-lexer-test-file-that-does-not-exist.rux'");
    CHECK(result.diagnostics[0].help->contains("readable"));
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

TEST_CASE("var is a keyword and mut is an ordinary identifier") {
    const auto result = Lex("var value = 1; let mut = value;");
    REQUIRE(result.diagnostics.empty());
    REQUIRE(result.tokens.size() >= 10);
    CHECK(result.tokens[0].Is(TokenKind::VarKeyword));
    CHECK(result.tokens[6].Is(TokenKind::Ident));
    CHECK(result.tokens[6].text == "mut");
}

TEST_CASE("Lexer uses maximal munch for logical right shift operators") {
    const auto result = Lex("a >>> b; a >>>= b;");
    REQUIRE(result.diagnostics.empty());
    REQUIRE(result.tokens.size() >= 8);
    CHECK(result.tokens[1].Is(TokenKind::GreaterGreaterGreater));
    CHECK(result.tokens[1].text == ">>>");
    CHECK(result.tokens[5].Is(TokenKind::GreaterGreaterGreaterAssign));
    CHECK(result.tokens[5].text == ">>>=");
}

TEST_CASE("Lexer recognizes ownership transfer arrows without splitting comparisons") {
    const auto result = Lex("let destination <- source; left < -right;");
    REQUIRE(result.diagnostics.empty());
    REQUIRE(result.tokens.size() >= 10);
    CHECK(result.tokens[2].Is(TokenKind::MoveArrow));
    CHECK_EQ(result.tokens[2].text, "<-");
    CHECK(result.tokens[6].Is(TokenKind::Less));
    CHECK(result.tokens[7].Is(TokenKind::Minus));
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
    CHECK(result.diagnostics.front().message == "escape sequence '\\q' is not recognized");
    CHECK(result.diagnostics.front().help->contains("'\\n'"));
    CHECK(result.diagnostics.front().documentationUrl == "https://rux-lang.dev/docs/");
}

TEST_CASE("Lexer accepts numeric bases, separators, exponents, and every suffix") {
    static constexpr std::string_view literals[] = {
        "0b1010_0101", "0o7_52", "0xFF_FF", "1_000", "1.25_00", "1e1_000", "1.5e+2", "1i",   "1i8",
        "1i16",        "1i32",   "1i64",    "1u",    "1u8",     "1u16",    "1u32",   "1u64", "1f8",
        "1f16",        "1f32",   "1f64",    "1f80",  "1f128",   "1f256",   "1f512",
    };
    for (const auto literal : literals) {
        CAPTURE(literal);
        CHECK_FALSE(Lex(std::string(literal)).HasErrors());
    }
}

TEST_CASE("Lexer identifies the precise numeric-literal failure") {
    struct Case {
        std::string_view source;
        std::string_view message;
        std::string_view help;
    };

    static constexpr Case cases[] = {
        {"0x", "hexadecimal literal requires at least one digit after '0x'", "0x2A"},
        {"0b", "binary literal requires at least one digit after '0b'", "0b101010"},
        {"0o", "octal literal requires at least one digit after '0o'", "0o52"},
        {"0b102", "digit '2' is not valid in a binary literal", "'0' and '1'"},
        {"0o78", "digit '8' is not valid in an octal literal", "'0' through '7'"},
        {"12_", "numeric separator '_' must appear between digits", "1_000"},
        {"1__2", "numeric separator '_' must appear between digits", "1_000"},
        {"1.2__5", "numeric separator '_' must appear between digits", "1.25_00"},
        {"1e+", "exponent requires at least one digit after 'e'", "1.5e+2"},
        {"12wat", "numeric literal suffix 'wat' is not recognized", "'i8'"},
    };
    for (const auto &test : cases) {
        CAPTURE(test.source);
        const auto result = Lex(std::string(test.source));
        REQUIRE_EQ(result.diagnostics.size(), 1);
        CHECK_EQ(result.diagnostics[0].message, test.message);
        REQUIRE(result.diagnostics[0].help.has_value());
        CHECK(result.diagnostics[0].help->contains(test.help));
    }
}

TEST_CASE("Lexer diagnoses comments, strings, characters, and Unicode escapes without cascades") {
    static constexpr std::pair<std::string_view, std::string_view> cases[] = {
        {"/* open", "block comment is not terminated"},
        {"\"open", "string literal is not terminated before the end of the file"},
        {"\"open\n", "string literal is not terminated before the end of the line"},
        {"\"open\\", "escape sequence is not complete before the end of the file"},
        {"''", "character literal is empty"},
        {"'ab'", "character literal contains more than one character"},
        {"'a", "character literal is not terminated before the end of the file"},
        {"'\\", "escape sequence is not complete before the end of the file"},
        {R"("\u";)", "Unicode escape requires '{' after '\\u'"},
        {R"("\u{}";)", "Unicode escape requires at least one hexadecimal digit"},
        {R"("\u{XYZ}";)", "Unicode escape contains a non-hexadecimal digit"},
        {R"("\u{D800}";)", "Unicode escape U+D800 is a surrogate, not a scalar value"},
        {R"("\u{110000}";)", "Unicode escape U+110000 is above the maximum scalar value U+10FFFF"},
        {R"("\u{)", "Unicode escape is not terminated before the end of the file"},
    };
    for (const auto &[source, message] : cases) {
        CAPTURE(source);
        const auto result = Lex(std::string(source));
        REQUIRE_EQ(result.diagnostics.size(), 1);
        CHECK_EQ(result.diagnostics.front().message, message);
    }
}

TEST_CASE("Lexer reports invalid UTF-8 and unexpected Unicode as one lexical cause") {
    const auto invalid = Lex(std::string("let x = ") + static_cast<char>(0xFF));
    REQUIRE_EQ(invalid.diagnostics.size(), 1);
    CHECK_EQ(invalid.diagnostics[0].message, "source contains invalid UTF-8 byte 0xFF");
    CHECK_EQ(invalid.diagnostics[0].location.column, 9);

    const auto unexpected = Lex("©");
    REQUIRE_EQ(unexpected.diagnostics.size(), 1);
    CHECK_EQ(unexpected.diagnostics[0].message, "unexpected character '©' (U+00A9)");
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
