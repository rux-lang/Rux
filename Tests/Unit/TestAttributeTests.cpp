// `#Test()` marks a function a test build runs; the parser records it and refuses it where no runner could call it.

#include "Lexer/Lexer.h"
#include "Syntax/Ast/Ast.h"
#include "Syntax/Parser/Parser.h"

#include <algorithm>
#include <doctest.h>
#include <string>
#include <utility>

using namespace Rux;

namespace {
ParseResult ParseSource(const std::string &source) {
    Lexer lexer(source, "test.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "test.rux");
    return parser.Parse();
}

bool Reports(const ParseResult &parsed, const std::string &message) {
    return std::ranges::any_of(parsed.diagnostics, [&](const Diagnostic &diagnostic) {
        return diagnostic.message.find(message) != std::string::npos;
    });
}
} // namespace

TEST_CASE("#Test() marks a function as a test") {
    const auto parsed = ParseSource(R"(
        #Test()
        func Adds() -> ! TestFailure {}

        func Helper() {}
    )");
    REQUIRE_FALSE(parsed.HasErrors());
    REQUIRE(parsed.module.items.size() == 2);
    const auto *test = dynamic_cast<const FuncDecl *>(parsed.module.items[0].get());
    const auto *helper = dynamic_cast<const FuncDecl *>(parsed.module.items[1].get());
    REQUIRE(test != nullptr);
    REQUIRE(helper != nullptr);
    CHECK(test->isTest);
    CHECK_FALSE(helper->isTest);
}

TEST_CASE("#Test() refuses arguments, duplicates, non-functions, parameters and generics") {
    const auto parsed = ParseSource(R"(
        #Test("named")
        func WithArgument() {}

        #Test()
        #Test()
        func Twice() {}

        #Test()
        const Value = 1;

        #Test()
        func Parameterized(count: int32) {}

        #Test()
        func Generic<T>() {}
    )");
    REQUIRE(parsed.HasErrors());
    CHECK(Reports(parsed, "'#Test' does not accept arguments"));
    CHECK(Reports(parsed, "duplicate '#Test' attribute"));
    CHECK(Reports(parsed, "'#Test' can only be applied to a function"));
    CHECK(Reports(parsed, "a '#Test' function takes no parameters"));
    CHECK(Reports(parsed, "a '#Test' function cannot be generic"));
}

TEST_CASE("#Test() refuses methods") {
    const auto parsed = ParseSource(R"(
        struct Counter { value: int32; }
        extend Counter {
            #Test()
            func Check(self: &Counter) {}
        }
    )");
    REQUIRE(parsed.HasErrors());
    CHECK(Reports(parsed, "a '#Test' function cannot be a method"));
}
