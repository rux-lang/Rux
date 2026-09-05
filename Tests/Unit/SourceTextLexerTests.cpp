#include "Lexer/Lexer.h"

#include <doctest.h>
#include <string>

using namespace Rux;

TEST_CASE("borrowed lexing owns every token and comment after the input is released") {
    std::string source = "/// An entry point\nfunc Main() -> int { /* value */ return 42; }\n";
    const auto owned = Lexer(source, "source.rux").Tokenize();
    const auto borrowed = Lexer::TokenizeSource(source, "source.rux");
    source.assign(source.size(), 'x');
    REQUIRE_FALSE(owned.HasErrors());
    REQUIRE_FALSE(borrowed.HasErrors());
    REQUIRE(borrowed.tokens.size() == owned.tokens.size());
    REQUIRE(borrowed.comments.size() == owned.comments.size());
    for (std::size_t i = 0; i < owned.tokens.size(); ++i) {
        CHECK(borrowed.tokens[i].kind == owned.tokens[i].kind);
        CHECK(borrowed.tokens[i].text == owned.tokens[i].text);
        CHECK(borrowed.tokens[i].location.offset == owned.tokens[i].location.offset);
    }
    for (std::size_t i = 0; i < owned.comments.size(); ++i) {
        CHECK(borrowed.comments[i].raw == owned.comments[i].raw);
        CHECK(borrowed.comments[i].kind == owned.comments[i].kind);
    }
}

TEST_CASE("copied owning lexers keep source storage alive") {
    auto lexer = [] {
        Lexer original("func Main() -> int { return 123; }");
        return Lexer(original);
    }();
    const auto result = lexer.Tokenize();
    CHECK_FALSE(result.HasErrors());
    REQUIRE(result.tokens.size() > 1);
    CHECK(result.tokens.front().text == "func");
}
