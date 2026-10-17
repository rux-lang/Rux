// Module variables: the declaration form, the rules for their initial value and type, and the uses that reach them.

#include "Lexer/Lexer.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <doctest.h>
#include <string>
#include <utility>
#include <vector>

using namespace Rux;

namespace {
ParseResult ParseSource(const std::string &source) {
    Lexer lexer(source, "module-variables.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "module-variables.rux");
    return parser.Parse();
}

std::vector<SemanticDiagnostic> AnalyzeSource(const std::string &source) {
    auto parsed = ParseSource(source);
    for (const auto &diagnostic : parsed.diagnostics) {
        INFO(diagnostic.message);
    }
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    return analyzer.Analyze().diagnostics;
}

bool HasMessage(const std::vector<SemanticDiagnostic> &diagnostics, const std::string &message) {
    for (const auto &diagnostic : diagnostics) {
        if (diagnostic.message == message) {
            return true;
        }
    }
    return false;
}

bool HasParseMessage(const ParseResult &parsed, const std::string &fragment) {
    for (const auto &diagnostic : parsed.diagnostics) {
        if (diagnostic.message.find(fragment) != std::string::npos) {
            return true;
        }
    }
    return false;
}
} // namespace

TEST_CASE("a module variable is declared with 'var', a type and an initial value") {
    auto parsed = ParseSource(R"(
        var Counter: int64 = 0;
        pub var Seed: uint32 = 7;
    )");
    REQUIRE_FALSE(parsed.HasErrors());
    REQUIRE_EQ(parsed.module.items.size(), 2);
    const auto *counter = dynamic_cast<const GlobalDecl *>(parsed.module.items[0].get());
    REQUIRE(counter != nullptr);
    CHECK_EQ(counter->name, "Counter");
    CHECK_FALSE(counter->isPublic);
    CHECK(counter->type != nullptr);
    CHECK(counter->value != nullptr);
    const auto *seed = dynamic_cast<const GlobalDecl *>(parsed.module.items[1].get());
    REQUIRE(seed != nullptr);
    CHECK(seed->isPublic);
}

TEST_CASE("a module variable needs its type and its initial value written out") {
    CHECK(HasParseMessage(ParseSource("var Counter = 0;"), "':' and the variable type"));
    CHECK(HasParseMessage(ParseSource("var Counter: int64;"), "'=' before the module variable's initial value"));
}

TEST_CASE("'let' at module scope is refused with the two declarations that are allowed") {
    const auto parsed = ParseSource("let Counter: int64 = 0;");
    REQUIRE(parsed.HasErrors());
    CHECK_EQ(parsed.diagnostics.front().message, "'let' is not allowed at module scope");
    CHECK_EQ(parsed.diagnostics.front().help,
             "use 'const' for a value that never changes, or 'var' for a module variable");
}

TEST_CASE("module variables are read, written and addressed from any function") {
    const auto diagnostics = AnalyzeSource(R"(
        struct Point { x: int32; y: int32; }
        const Start: int = 5;
        var Total: int = Start * 2;
        var Origin: Point = Point { x: 1, y: 2 };
        var Table: int32[3] = [1, 2, 3];

        module Settings {
            pub var Level: int32 = 3;
        }

        func Add(amount: int) {
            Total = Total + amount;
            Total += 1;
            Total++;
            Origin.x = 4;
            Table[1] = 9;
            Settings::Level = 7;
            Settings::Level += 1;
            let address: *var int = @Total;
            *address = 3;
        }

        func Main() -> int {
            Add(1);
            return Total;
        }
    )");
    for (const auto &diagnostic : diagnostics) {
        INFO(diagnostic.message);
    }
    CHECK(diagnostics.empty());
}

TEST_CASE("a module variable's initial value is computed while compiling") {
    const auto diagnostics = AnalyzeSource(R"(
        func Seed() -> int { return 4; }
        var First: int = 1;
        var Second: int = First + 1;
        var Third: int = Seed();
        func Main() -> int { return 0; }
    )");
    CHECK(HasMessage(diagnostics, "'First' is not a compile-time constant"));
    CHECK(HasMessage(diagnostics, "call to 'Seed' is not a compile-time value"));
}

TEST_CASE("a module variable's initial value must have its declared type") {
    const auto diagnostics = AnalyzeSource(R"(
        var Flag: bool = 3;
        func Main() -> int { return 0; }
    )");
    REQUIRE_FALSE(diagnostics.empty());
    CHECK(diagnostics.front().message.find("module variable of type 'bool8'") != std::string::npos);
}

TEST_CASE("a module variable cannot hold a value that needs destruction or a borrowed view") {
    const auto diagnostics = AnalyzeSource(R"(
        struct Handle { value: int; }
        extend Handle {
            func ~Handle(self: &var Handle) {}
        }
        var Open: Handle = Handle { value: 1 };
        var Name: char8[..] = "name";
        func Main() -> int { return 0; }
    )");
    CHECK(HasMessage(diagnostics, "module variable 'Open' has type 'Handle', which needs destruction"));
    CHECK(HasMessage(diagnostics, "module variable 'Name' cannot have slice type 'char8[..]'"));
}

TEST_CASE("nothing moves out of a module variable") {
    const auto diagnostics = AnalyzeSource(R"(
        struct Pair { left: int; right: int; }
        var Shared: Pair = Pair { left: 1, right: 2 };
        func Main() -> int {
            let taken = <-Shared;
            return taken.left;
        }
    )");
    CHECK(HasMessage(diagnostics, "cannot move out of module variable 'Shared'"));
}

TEST_CASE("a module variable is declared at module scope, not inside a function") {
    const auto diagnostics = AnalyzeSource(R"(
        func Main() -> int {
            pub var Inner: int = 1;
            return 0;
        }
    )");
    CHECK(HasMessage(diagnostics, "module variable 'Inner' must be declared at module scope"));
}

TEST_CASE("a public module variable cannot expose a private type") {
    const auto diagnostics = AnalyzeSource(R"(
        struct Secret { value: int; }
        pub var Exposed: Secret = Secret { value: 1 };
        func Main() -> int { return 0; }
    )");
    bool reported = false;
    for (const auto &diagnostic : diagnostics) {
        reported = reported || diagnostic.message.find("public module variable 'Exposed'") != std::string::npos;
    }
    CHECK(reported);
}

TEST_CASE("'thread var' declares a thread-local module variable") {
    auto parsed = ParseSource(R"(
        thread var Depth: int64 = 0;
        pub thread var Seed: uint32 = 7;
        var Shared: int64 = 1;
    )");
    REQUIRE_FALSE(parsed.HasErrors());
    REQUIRE_EQ(parsed.module.items.size(), 3);
    const auto *depth = dynamic_cast<const GlobalDecl *>(parsed.module.items[0].get());
    const auto *seed = dynamic_cast<const GlobalDecl *>(parsed.module.items[1].get());
    const auto *shared = dynamic_cast<const GlobalDecl *>(parsed.module.items[2].get());
    REQUIRE(depth != nullptr);
    REQUIRE(seed != nullptr);
    REQUIRE(shared != nullptr);
    CHECK(depth->isThreadLocal);
    CHECK_FALSE(depth->isPublic);
    CHECK(seed->isThreadLocal);
    CHECK(seed->isPublic);
    CHECK_FALSE(shared->isThreadLocal);
}

TEST_CASE("'thread' stays an ordinary name everywhere but before a module 'var'") {
    const auto diagnostics = AnalyzeSource(R"(
        var thread: int = 1;
        thread var Count: int = 0;
        func Main() -> int {
            let thread: int = 2;
            Count = Count + thread;
            return Count;
        }
    )");
    CHECK(diagnostics.empty());
}

TEST_CASE("a thread-local variable follows the rules of a module variable") {
    const auto diagnostics = AnalyzeSource(R"(
        struct Handle { value: int; }
        extend Handle {
            func ~Handle(self: &var Handle) {}
        }
        func Seed() -> int { return 1; }
        thread var Open: Handle = Handle { value: 1 };
        thread var First: int = Seed();
        func Main() -> int { return First; }
    )");
    CHECK(HasMessage(diagnostics, "module variable 'Open' has type 'Handle', which needs destruction"));
    CHECK(HasMessage(diagnostics, "call to 'Seed' is not a compile-time value"));
}
