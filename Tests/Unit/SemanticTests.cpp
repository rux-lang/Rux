#include "Lexer/Lexer.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <algorithm>
#include <array>
#include <doctest.h>
#include <string>
#include <string_view>
#include <vector>

using namespace Rux;

namespace {

std::vector<SemanticDiagnostic> AnalyzeSource(const std::string &source) {
    Lexer lexer(source, "test.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());

    Parser parser(std::move(lexed.tokens), "test.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    return analyzer.Analyze().diagnostics;
}

// Analyze `userSource` with a single dependency package `depName` whose source
// is `depSource`. The parsed modules stay alive for the whole Analyze() call.
std::vector<SemanticDiagnostic> AnalyzeWithDep(const std::string &userSource, const std::string &depName,
                                               const std::string &depSource) {
    Lexer depLexer(depSource, "dep.rux");
    auto depLexed = depLexer.Tokenize();
    REQUIRE_FALSE(depLexed.HasErrors());
    Parser depParser(std::move(depLexed.tokens), "dep.rux");
    auto depParsed = depParser.Parse();
    REQUIRE_FALSE(depParsed.HasErrors());

    Lexer lexer(userSource, "test.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "test.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    DepPackage dep;
    dep.name = depName;
    dep.modules.push_back({depName, &depParsed.module});

    SemanticAnalyzer analyzer({&parsed.module}, {std::move(dep)}, "App", "Windows");
    return analyzer.Analyze().diagnostics;
}

} // namespace

TEST_CASE("extern function call attributes emit direct and qualified diagnostics") {
    const auto diagnostics = AnalyzeSource(R"(
        #Error("direct extern call is forbidden")
        #Link("Kernel32.dll")
        extern func Beep(freq: uint32, duration: uint32) -> bool32;

        module Native {
            #Warn("qualified extern call is discouraged")
            #Link("Kernel32.dll")
            extern func Sleep(milliseconds: uint32);
        }

        func Main() -> int {
            Beep(1000u32, 500u32);
            Native::Sleep(1u32);
            return 0;
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 2);
    CHECK_EQ(diagnostics[0].severity, Diagnostic::Severity::Error);
    CHECK_EQ(diagnostics[0].message, "direct extern call is forbidden");
    CHECK_EQ(diagnostics[1].severity, Diagnostic::Severity::Warning);
    CHECK_EQ(diagnostics[1].message, "qualified extern call is discouraged");
}

TEST_CASE("one-argument Link applies a library to every function in an extern block") {
    const auto diagnostics = AnalyzeSource(R"(
        #Link("Kernel32.dll")
        extern {
            func Beep(freq: uint32, duration: uint32) -> bool32;
            func Sleep(milliseconds: uint32);
        }
    )");

    CHECK(diagnostics.empty());
}

TEST_CASE("duplicate free-function signatures are rejected") {
    const auto diagnostics = AnalyzeSource(R"(
        func Do() {}
        func Do() {}

        func Convert(value: int) {}
        func Convert(value: uint) {}
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics[0].severity, Diagnostic::Severity::Error);
    CHECK_EQ(diagnostics[0].location.line, 3);
    CHECK_EQ(diagnostics[0].message, "function 'Do' has the same parameter signature as a previous declaration at 2:9");
}

TEST_CASE("duplicate method signatures are rejected") {
    const auto diagnostics = AnalyzeSource(R"(
        struct Item {}

        extend Item {
            func Run(self) {}
            func Run(self) {}
            func Run(self, value: int) {}
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics[0].severity, Diagnostic::Severity::Error);
    CHECK_EQ(diagnostics[0].location.line, 6);
    CHECK_EQ(diagnostics[0].message,
             "function 'Run' has the same parameter signature as a previous declaration at 5:13");
}

TEST_CASE("same function signature in distinct modules remains valid") {
    const auto diagnostics = AnalyzeSource(R"(
        module First {
            func Do() {}
        }
        module Second {
            func Do() {}
        }
    )");

    CHECK(diagnostics.empty());
}

TEST_CASE("documented primitive names report when their implementation is unavailable") {
    constexpr std::array<std::string_view, 20> types{
        "int128",   "int256",   "int512", "uint128", "uint256", "uint512", "float8", "float16", "float80", "float128",
        "float256", "float512", "bool64", "bool128", "bool256", "bool512", "char64", "char128", "char256", "char512",
    };

    std::string source;
    for (std::size_t i = 0; i < types.size(); ++i) {
        source += "struct Holder" + std::to_string(i) + " { value: " + std::string(types[i]) + "; }\n";
    }

    const auto diagnostics = AnalyzeSource(source);
    REQUIRE_EQ(diagnostics.size(), types.size());
    for (std::size_t i = 0; i < types.size(); ++i) {
        CAPTURE(types[i]);
        CHECK_EQ(diagnostics[i].severity, Diagnostic::Severity::Error);
        CHECK_EQ(diagnostics[i].message, "primitive type '" + std::string(types[i]) +
                                             "' is reserved but is not implemented in this compiler version");
    }
}

TEST_CASE("unimplemented primitive names cannot be declared as user types") {
    const auto diagnostics = AnalyzeSource("struct int128 {}");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK(diagnostics.front().message.starts_with("'int128' is already defined"));
}

TEST_CASE("ordinary unknown types keep the unknown-type diagnostic") {
    const auto diagnostics = AnalyzeSource("struct Holder { value: CustomInteger; }");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics.front().message, "unknown type 'CustomInteger'");
}

TEST_CASE("a flexible array is accepted only as the final struct field") {
    CHECK(AnalyzeSource(R"(
        struct Packet {
            length: uint;
            data: uint8[];
        }
    )")
              .empty());

    const auto diagnostics = AnalyzeSource(R"(
        struct NotTail {
            data: uint8[];
            length: uint;
        }
        union NotStruct { data: uint8[] }
        func Invalid(value: uint8[]) -> uint8[] {
            let mut local: uint8[];
            return value;
        }
    )");

    CHECK_EQ(std::ranges::count_if(diagnostics,
                                   [](const SemanticDiagnostic &diagnostic) {
                                       return diagnostic.message ==
                                              "flexible array type is only allowed as the final field of a struct";
                                   }),
             5);
}

TEST_CASE("fixed arrays require matching literal extents") {
    CHECK(AnalyzeSource(R"(
        const Bytes: uint8[3] = [1u8, 2u8, 3u8];
        func Main() {
            let mut values: uint16[2] = [10u16, 20u16];
            values[1] = 30u16;
        }
    )")
              .empty());

    const auto diagnostics = AnalyzeSource("const Bytes: uint8[2] = [1u8, 2u8, 3u8];");
    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK(diagnostics.front().message.find("cannot assign") != std::string::npos);
}

TEST_CASE("prefix operators bind more tightly than casts") {
    const auto diagnostics = AnalyzeSource(R"(
        func Main() {
            let value = 10;
            let pointer = @value;
            let address: uint = @value as uint;
        }
    )");

    CHECK(diagnostics.empty());
}

TEST_CASE("pointer and array type syntax preserves grouping") {
    CHECK(AnalyzeSource(R"(
        func Main() {
            let values: uint[4] = [255u, 127u, 10u, 0u];
            let pointerToArray: *(uint[4]) = @values;
            let arrayOfPointers: (*uint)[2] = [@values[0], @values[1]];
            let oneTuple: (uint,) = (1u,);
        }
    )")
              .empty());

    const auto diagnostics = AnalyzeSource(R"(
        func Main() {
            let values: uint[4] = [255u, 127u, 10u, 0u];
            let wrong: (*uint)[4] = @values;
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics.front().message, "cannot assign '*(uint[4])' to '(*uint)[4]'");
}

TEST_CASE("bare package import binds the eponymous module for qualified access") {
    const auto diagnostics = AnalyzeWithDep(R"(
        import Platform;

        func Main() -> int {
            return Platform::Now();
        }
    )",
                                            "Platform", R"(
        module Platform {
            func Now() -> int { return 7; }
        }
    )");

    CHECK(diagnostics.empty());
}

TEST_CASE("bare package import without an eponymous module is an error") {
    const auto diagnostics = AnalyzeWithDep(R"(
        import Utils;

        func Main() -> int { return 0; }
    )",
                                            "Utils", R"(
        module Helpers {
            func Ping() -> int { return 1; }
        }
    )");

    const bool reported = std::ranges::any_of(diagnostics, [](const SemanticDiagnostic &d) {
        return d.severity == Diagnostic::Severity::Error &&
               d.message == "import 'Utils' does not name a module; name an item instead (e.g. import Utils::Name)";
    });
    CHECK(reported);
}

TEST_CASE("importing a module's item without naming the module is an error") {
    const auto diagnostics = AnalyzeWithDep(R"(
        import Foo::Bar;

        func Main() -> int { return 0; }
    )",
                                            "Foo", R"(
        module Foo {
            func Bar() -> int { return 7; }
        }
    )");

    const bool reported = std::ranges::any_of(diagnostics, [](const SemanticDiagnostic &d) {
        return d.severity == Diagnostic::Severity::Error &&
               d.message == "'Bar' not found in package 'Foo'; did you mean 'import Foo::Foo::Bar'?";
    });
    CHECK(reported);
}

TEST_CASE("importing a module's item through its full path resolves") {
    const auto diagnostics = AnalyzeWithDep(R"(
        import Foo::Foo::Bar;

        func Main() -> int {
            return Bar();
        }
    )",
                                            "Foo", R"(
        module Foo {
            func Bar() -> int { return 7; }
        }
    )");

    CHECK(diagnostics.empty());
}
