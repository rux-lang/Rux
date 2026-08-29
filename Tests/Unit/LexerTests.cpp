#include "Lexer/Lexer.h"

#include <doctest.h>
#include <string>
#include <utility>
#include <vector>

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

TEST_CASE("Lexer classifies only exact documentation comment markers") {
    const auto result = Lex("// ordinary\n"
                            "//// decorative line\n"
                            "/**/ /*** decorative block */\n"
                            "/// line documentation\n"
                            "/** block documentation */\n"
                            "let value = 1;\n");

    REQUIRE(result.diagnostics.empty());
    REQUIRE_EQ(result.comments.size(), 6);
    CHECK(result.comments[0].kind == CommentKind::Line);
    CHECK(result.comments[1].kind == CommentKind::Line);
    CHECK(result.comments[2].kind == CommentKind::Block);
    CHECK(result.comments[3].kind == CommentKind::Block);
    CHECK(result.comments[4].kind == CommentKind::DocumentationLine);
    CHECK(result.comments[5].kind == CommentKind::DocumentationBlock);

    CHECK_EQ(result.comments[0].raw, "// ordinary");
    CHECK_EQ(result.comments[1].raw, "//// decorative line");
    CHECK_EQ(result.comments[2].raw, "/**/");
    CHECK_EQ(result.comments[3].raw, "/*** decorative block */");
    CHECK_EQ(result.comments[4].raw, "/// line documentation");
    CHECK_EQ(result.comments[5].raw, "/** block documentation */");

    std::vector<std::string> documentationTokens;
    for (const auto &token : result.tokens) {
        if (token.Is(TokenKind::DocComment)) {
            documentationTokens.push_back(token.text);
        }
    }
    REQUIRE_EQ(documentationTokens.size(), 2);
    CHECK_EQ(documentationTokens[0], "/// line documentation");
    CHECK_EQ(documentationTokens[1], "/** block documentation */");
}

TEST_CASE("Lexer records lossless half-open comment ranges") {
    const std::string source = "  // first\r\n"
                               "value /* second\n"
                               " nested */\n"
                               "\t/// third\r\n"
                               "/** fourth\n"
                               " * line\n"
                               " */tail";
    const auto result = Lex(source);

    REQUIRE(result.diagnostics.empty());
    REQUIRE_EQ(result.comments.size(), 4);
    for (const auto &comment : result.comments) {
        REQUIRE(comment.range.end.offset >= comment.range.start.offset);
        CHECK_EQ(comment.range.Length(), comment.raw.size());
        CHECK_EQ(source.substr(comment.range.start.offset, comment.range.Length()), comment.raw);
        CHECK_FALSE(comment.range.Empty());
        CHECK(comment.terminated);
    }

    CHECK(result.comments[0].range.start == SourceLocation{1, 3, 2});
    CHECK(result.comments[0].range.end == SourceLocation{1, 11, 10});
    CHECK(result.comments[1].range.start.line == 2);
    CHECK(result.comments[1].range.start.column == 7);
    CHECK(result.comments[1].range.end.line == 3);
    CHECK(result.comments[2].range.start.line == 4);
    CHECK(result.comments[2].range.start.column == 2);
    CHECK_EQ(result.comments[2].raw, "/// third");
    CHECK(result.comments[3].range.start.line == 5);
    CHECK(result.comments[3].range.end.line == 7);

    CHECK(result.comments[0].lineLeading);
    CHECK_FALSE(result.comments[1].lineLeading);
    CHECK(result.comments[2].lineLeading);
    CHECK(result.comments[3].lineLeading);
}

TEST_CASE("Documentation tokens retain attachment metadata") {
    const auto result = Lex("value; /// trailing\n"
                            "/* divider */\n"
                            "/// leading\n"
                            "/** block\n"
                            " */\n"
                            "func Next();\n");
    REQUIRE(result.diagnostics.empty());
    REQUIRE_EQ(result.comments.size(), 4);

    std::vector<const Token *> documentation;
    for (const auto &token : result.tokens) {
        if (token.Is(TokenKind::DocComment)) {
            documentation.push_back(&token);
        }
    }
    REQUIRE_EQ(documentation.size(), 3);

    CHECK_EQ(documentation[0]->text, "/// trailing");
    CHECK_FALSE(documentation[0]->lineLeading);
    CHECK_FALSE(documentation[0]->precededByOrdinaryComment);
    CHECK(documentation[0]->location == SourceLocation{1, 8, 7});
    CHECK(documentation[0]->endLocation == SourceLocation{1, 20, 19});

    CHECK_EQ(documentation[1]->text, "/// leading");
    CHECK(documentation[1]->lineLeading);
    CHECK(documentation[1]->precededByOrdinaryComment);
    CHECK(documentation[1]->location == result.comments[2].range.start);
    CHECK(documentation[1]->endLocation == result.comments[2].range.end);

    CHECK_EQ(documentation[2]->text, "/** block\n */");
    CHECK(documentation[2]->lineLeading);
    CHECK_FALSE(documentation[2]->precededByOrdinaryComment);
    CHECK(documentation[2]->location == result.comments[3].range.start);
    CHECK(documentation[2]->endLocation == result.comments[3].range.end);
}

TEST_CASE("Ordinary and documentation block comments nest") {
    const auto result = Lex("/* outer /* nested */ ordinary */\n"
                            "/** outer /* nested */ documentation */\n"
                            "func Done();\n");
    REQUIRE(result.diagnostics.empty());
    REQUIRE_EQ(result.comments.size(), 2);
    CHECK(result.comments[0].kind == CommentKind::Block);
    CHECK(result.comments[1].kind == CommentKind::DocumentationBlock);
    CHECK_EQ(result.comments[0].raw, "/* outer /* nested */ ordinary */");
    CHECK_EQ(result.comments[1].raw, "/** outer /* nested */ documentation */");
    CHECK(result.comments[0].terminated);
    CHECK(result.comments[1].terminated);

    REQUIRE(!result.tokens.empty());
    CHECK(result.tokens[0].Is(TokenKind::DocComment));
    CHECK_EQ(result.tokens[0].text, result.comments[1].raw);
}

TEST_CASE("Lexer retains unterminated ordinary and documentation blocks") {
    SUBCASE("ordinary block") {
        const auto result = Lex("/* still open");
        REQUIRE(result.HasErrors());
        REQUIRE_EQ(result.comments.size(), 1);
        CHECK(result.comments[0].kind == CommentKind::Block);
        CHECK_EQ(result.comments[0].raw, "/* still open");
        CHECK_FALSE(result.comments[0].terminated);
        CHECK_EQ(result.comments[0].range.end.offset, 13);
        CHECK(result.tokens.back().IsEof());
    }

    SUBCASE("documentation block") {
        const auto result = Lex("/** still open");
        REQUIRE(result.HasErrors());
        REQUIRE_EQ(result.comments.size(), 1);
        CHECK(result.comments[0].kind == CommentKind::DocumentationBlock);
        CHECK_EQ(result.comments[0].raw, "/** still open");
        CHECK_FALSE(result.comments[0].terminated);
        REQUIRE_EQ(result.tokens.size(), 2);
        CHECK(result.tokens[0].Is(TokenKind::DocComment));
        CHECK_EQ(result.tokens[0].endLocation.offset, 14);
        CHECK(result.tokens.back().IsEof());
    }
}

TEST_CASE("Comment delimiters in literals do not produce trivia") {
    const auto result = Lex(R"(let a = "/// not documentation";)"
                            R"(let b = "/** not a block */";)"
                            R"(let c = "/* ordinary */ // still text";)"
                            "\n// actual comment\n");
    REQUIRE(result.diagnostics.empty());
    REQUIRE_EQ(result.comments.size(), 1);
    CHECK(result.comments[0].kind == CommentKind::Line);
    CHECK_EQ(result.comments[0].raw, "// actual comment");

    std::size_t stringCount = 0;
    for (const auto &token : result.tokens) {
        if (token.Is(TokenKind::StringLiteral)) {
            ++stringCount;
        }
        CHECK_FALSE(token.Is(TokenKind::DocComment));
    }
    CHECK_EQ(stringCount, 3);
}

TEST_CASE("Comments preserve spacing-sensitive token separation") {
    const auto result = Lex("a?; a /* gap */ ?; [] [/* gap */] []= [/* gap */] =");
    REQUIRE(result.diagnostics.empty());
    REQUIRE_EQ(result.comments.size(), 3);
    REQUIRE_EQ(result.tokens.size(), 17);

    CHECK(result.tokens[1].Is(TokenKind::Question));
    CHECK_FALSE(result.tokens[1].precededBySpace);
    CHECK(result.tokens[4].Is(TokenKind::Question));
    CHECK(result.tokens[4].precededBySpace);

    CHECK(result.tokens[7].Is(TokenKind::RightBracket));
    CHECK_FALSE(result.tokens[7].precededBySpace);
    CHECK(result.tokens[9].Is(TokenKind::RightBracket));
    CHECK(result.tokens[9].precededBySpace);
    CHECK(result.tokens[11].Is(TokenKind::RightBracket));
    CHECK_FALSE(result.tokens[11].precededBySpace);
    CHECK(result.tokens[12].Is(TokenKind::Assign));
    CHECK_FALSE(result.tokens[12].precededBySpace);
    CHECK(result.tokens[14].Is(TokenKind::RightBracket));
    CHECK(result.tokens[14].precededBySpace);
    CHECK(result.tokens[15].Is(TokenKind::Assign));
    CHECK(result.tokens[15].precededBySpace);
}

TEST_CASE("Line comment ranges exclude LF and CRLF terminators") {
    const std::string source = "/// windows\r\n"
                               "// posix\n"
                               "let value = 0;\n";
    const auto result = Lex(source);
    REQUIRE(result.diagnostics.empty());
    REQUIRE_EQ(result.comments.size(), 2);

    CHECK_EQ(result.comments[0].raw, "/// windows");
    CHECK_EQ(result.comments[0].range.end.offset, source.find('\r'));
    CHECK(result.comments[0].range.end == SourceLocation{1, 12, 11});
    CHECK_EQ(result.comments[1].raw, "// posix");
    CHECK_EQ(result.comments[1].range.end.offset, source.find('\n', source.find('\n') + 1));
    CHECK(result.comments[1].range.end.line == 2);

    REQUIRE(result.tokens[0].Is(TokenKind::DocComment));
    CHECK(result.tokens[0].endLocation == result.comments[0].range.end);
    CHECK(result.tokens[1].Is(TokenKind::LetKeyword));
    CHECK_EQ(result.tokens[1].location.line, 3);
}

TEST_CASE("Empty documentation forms remain exact lossless comments") {
    const auto result = Lex("///\n"
                            "/// \n"
                            "/** */\n"
                            "/**\n"
                            " */\n");
    REQUIRE(result.diagnostics.empty());
    REQUIRE_EQ(result.comments.size(), 4);
    REQUIRE_EQ(result.tokens.size(), 5);

    static constexpr std::string_view spellings[] = {"///", "/// ", "/** */", "/**\n */"};
    for (std::size_t index = 0; index < std::size(spellings); ++index) {
        CAPTURE(index);
        CHECK(IsDocumentationComment(result.comments[index].kind));
        CHECK_EQ(result.comments[index].raw, spellings[index]);
        CHECK(result.tokens[index].Is(TokenKind::DocComment));
        CHECK_EQ(result.tokens[index].text, spellings[index]);
        CHECK(result.tokens[index].location == result.comments[index].range.start);
        CHECK(result.tokens[index].endLocation == result.comments[index].range.end);
    }
}

TEST_CASE("Lexer assigns half-open ranges to ordinary tokens") {
    const std::string source = "name += \"value\";";
    const auto result = Lex(source);
    REQUIRE(result.diagnostics.empty());
    REQUIRE_EQ(result.tokens.size(), 5);

    for (const auto &token : result.tokens) {
        REQUIRE(token.endLocation.offset >= token.location.offset);
        if (!token.IsEof()) {
            const std::uint32_t length = token.endLocation.offset - token.location.offset;
            CHECK_EQ(source.substr(token.location.offset, length), token.text);
        }
    }
    CHECK(result.tokens.back().location == result.tokens.back().endLocation);
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

TEST_CASE("Lexer uses maximal munch for Option coalescing without changing postfix question") {
    const auto result = Lex("a?; b??c; d???e;");
    REQUIRE(result.diagnostics.empty());
    REQUIRE_EQ(result.tokens.size(), 13);
    CHECK(result.tokens[1].Is(TokenKind::Question));
    CHECK(result.tokens[4].Is(TokenKind::QuestionQuestion));
    CHECK_EQ(result.tokens[4].text, "??");
    CHECK_FALSE(result.tokens[4].IsOperator());
    CHECK(result.tokens[8].Is(TokenKind::QuestionQuestion));
    CHECK(result.tokens[9].Is(TokenKind::Question));
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

TEST_CASE("Lexer keeps adjacent stars as separate multiplication and dereference tokens") {
    const auto result = Lex("left**right; left * *right;");
    REQUIRE(result.diagnostics.empty());
    REQUIRE_EQ(result.tokens.size(), 11);
    for (const std::size_t index : {1u, 2u, 6u, 7u}) {
        CHECK(result.tokens[index].Is(TokenKind::Star));
        CHECK_EQ(result.tokens[index].text, "*");
    }
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
    CHECK(KeywordKind("enum") == TokenKind::EnumKeyword);
    CHECK(KeywordKind("variant") == TokenKind::VariantKeyword);
    CHECK(TokenKindName(TokenKind::VariantKeyword) == "VariantKeyword");
    CHECK(KeywordKind("while") == TokenKind::WhileKeyword);
    CHECK(KeywordKind("if") == TokenKind::IfKeyword);
    CHECK(KeywordKind("when") == TokenKind::WhenKeyword);
    CHECK(KeywordKind("super") == TokenKind::Ident);
    CHECK(KeywordKind("funcy") == TokenKind::Ident);
    CHECK(KeywordKind("whenever") == TokenKind::Ident);
    CHECK(KeywordKind("") == TokenKind::Ident);
}

TEST_CASE("Token keyword predicate covers the alphabetized keyword range") {
    for (std::uint8_t value = static_cast<std::uint8_t>(TokenKind::AsKeyword);
         value <= static_cast<std::uint8_t>(TokenKind::WhileKeyword); ++value) {
        Token token;
        token.kind = static_cast<TokenKind>(value);
        CHECK(token.IsKeyword());
    }

    Token identifier;
    identifier.kind = TokenKind::Ident;
    CHECK_FALSE(identifier.IsKeyword());

    Token punctuation;
    punctuation.kind = TokenKind::LeftParen;
    CHECK_FALSE(punctuation.IsKeyword());
}

TEST_CASE("variant is reserved without capturing longer identifiers") {
    const auto result = Lex("variant Result { Value } variantValue variant_result variants");
    REQUIRE(result.diagnostics.empty());
    REQUIRE_EQ(result.tokens.size(), 9);

    CHECK(result.tokens[0].Is(TokenKind::VariantKeyword));
    CHECK_EQ(result.tokens[0].text, "variant");
    CHECK(result.tokens[1].Is(TokenKind::Ident));
    CHECK_EQ(result.tokens[1].text, "Result");
    CHECK(result.tokens[2].Is(TokenKind::LeftBrace));
    CHECK(result.tokens[3].Is(TokenKind::Ident));
    CHECK_EQ(result.tokens[3].text, "Value");
    CHECK(result.tokens[4].Is(TokenKind::RightBrace));

    CHECK(result.tokens[5].Is(TokenKind::Ident));
    CHECK_EQ(result.tokens[5].text, "variantValue");
    CHECK(result.tokens[6].Is(TokenKind::Ident));
    CHECK_EQ(result.tokens[6].text, "variant_result");
    CHECK(result.tokens[7].Is(TokenKind::Ident));
    CHECK_EQ(result.tokens[7].text, "variants");
    CHECK(result.tokens.back().IsEof());
}
