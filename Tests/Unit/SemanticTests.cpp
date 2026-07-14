#include "Lexer/Lexer.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

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
