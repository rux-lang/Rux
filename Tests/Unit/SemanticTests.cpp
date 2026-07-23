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

TEST_CASE("let and var independently control binding and pointee mutability") {
    const auto diagnostics = AnalyzeSource(R"(
        func Main() {
            let immutable = 10;
            var mutable = 20;

            let readOnly: *int = @immutable;
            let writable: *var int = @mutable;
            let weakened: *int = @mutable;

            immutable = 11;
            *readOnly = 12;
            *writable = 21;
            mutable = 22;

            let bad: *var int = @immutable;
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 3);
    CHECK_EQ(diagnostics[0].message, "cannot assign to immutable variable 'immutable'");
    CHECK_EQ(diagnostics[1].message, "cannot assign through a pointer to immutable data");
    CHECK_EQ(diagnostics[2].message, "cannot assign '*int' to '*var int': '@immutable' yields a read-only '*T'; "
                                     "declare 'immutable' with 'var' for a '*var T'");
}

TEST_CASE("function parameters are immutable unless declared var") {
    const auto diagnostics = AnalyzeSource(R"(
        func Immutable(x: int, ptr: *var int) {
            x = 1;
            ptr = ptr;
            *ptr = 2;
        }

        func Mutable(var x: int, var ptr: *var int) {
            x = 3;
            ptr = ptr;
            *ptr = 4;
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 2);
    CHECK_EQ(diagnostics[0].message, "cannot assign to immutable variable 'x'");
    CHECK_EQ(diagnostics[1].message, "cannot assign to immutable variable 'ptr'");
}

TEST_CASE("pointer binding mutability is independent of pointee mutability") {
    const auto diagnostics = AnalyzeSource(R"(
        func Main() {
            let a = 10;
            var b = 20;

            let immutableReadOnly: *int = @a;
            let immutableWritable: *var int = @b;
            var mutableReadOnly: *int = @a;
            var mutableWritable: *var int = @b;

            immutableReadOnly = mutableReadOnly;
            immutableWritable = mutableWritable;
            mutableReadOnly = immutableReadOnly;
            mutableWritable = immutableWritable;

            *immutableWritable = 21;
            *mutableWritable = 22;
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 2);
    CHECK_EQ(diagnostics[0].message, "cannot assign to immutable variable 'immutableReadOnly'");
    CHECK_EQ(diagnostics[1].message, "cannot assign to immutable variable 'immutableWritable'");
}

TEST_CASE("byte is a canonical alias of uint8") {
    const auto diagnostics = AnalyzeSource(R"(
        func Read(value: uint8) -> byte {
            return value;
        }

        func Main() {
            let raw: byte = 255u8;
            let numeric: uint8 = raw;
            var storage: byte[2] = [raw, numeric];
            let ptr: *var byte = @storage[0];
            *ptr = Read(1u8);
        }
    )");

    CHECK(diagnostics.empty());
}

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
            var local: uint8[];
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
            var values: uint16[2] = [10u16, 20u16];
            values[1] = 30u16;
        }
    )")
              .empty());

    const auto diagnostics = AnalyzeSource("const Bytes: uint8[2] = [1u8, 2u8, 3u8];");
    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK(diagnostics.front().message.find("cannot assign") != std::string::npos);
}

TEST_CASE("contextual enum patterns infer generic subject types") {
    const auto diagnostics = AnalyzeSource(R"(
        enum Result<T, E> {
            Success(T),
            Error(E)
        }

        enum ParseError {
            Invalid
        }

        func Unwrap(result: Result<float64, ParseError>) -> float64 {
            return match result {
                .Success(value) => value,
                .Error(_) => 0.0
            };
        }
    )");

    CHECK(diagnostics.empty());
}

TEST_CASE("generic arithmetic is checked after type substitution") {
    CHECK(AnalyzeSource(R"(
        func Div<T>(x: T, y: T) -> T {
            return x / y;
        }

        func Forward<T>(x: T, y: T) -> T {
            return Div<T>(x, y);
        }

        func Main() {
            let quotient = Forward<float>(10.0, 2.0);
        }
    )")
              .empty());

    const auto diagnostics = AnalyzeSource(R"(
        func Div<T>(x: T, y: T) -> T {
            return x / y;
        }

        func Forward<T>(x: T, y: T) -> T {
            return Div<T>(x, y);
        }

        func Main() {
            let quotient = Forward<bool>(true, false);
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics.front().message, "'/' applied to non-numeric type 'bool8'");
}

TEST_CASE("contextual enum patterns diagnose unknown variants") {
    const auto diagnostics = AnalyzeSource(R"(
        enum Option {
            Some(int),
            None
        }

        func Read(option: Option) -> int {
            return match option {
                .Missing => 0,
                else => 1
            };
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics.front().message, "enum 'Option' has no variant 'Missing'");
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

TEST_CASE("logical right shift requires a signed integer left operand") {
    CHECK(AnalyzeSource(R"(
        func Main() {
            let value: int8 = -8;
            let shifted: int8 = value >>> 2;
        }
    )")
              .empty());

    const auto diagnostics = AnalyzeSource(R"(
        func Main() {
            let value: uint8 = 248;
            let shifted = value >>> 2;
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics.front().message, "'>>>' requires a signed integer left operand, got 'uint8'");

    const auto compoundDiagnostics = AnalyzeSource(R"(
        func Main() {
            var value: uint8 = 248;
            value >>>= 2;
        }
    )");

    REQUIRE_EQ(compoundDiagnostics.size(), 1);
    CHECK_EQ(compoundDiagnostics.front().message, "'>>>=' requires a signed integer target, got 'uint8'");
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

TEST_CASE("all six range expressions type-check for collection slicing") {
    const auto diagnostics = AnalyzeSource(R"(
        func Main() {
            let values: int[6] = [10, 20, 30, 40, 50, 60];
            let bounded = values[2..4];
            let inclusive = values[2..=4];
            let from = values[2..];
            let to = values[..3];
            let toInclusive = values[..=3];
            let full = values[..];
        }
    )");

    CHECK(diagnostics.empty());
}

TEST_CASE("ranges without a start are not independently iterable") {
    const auto diagnostics = AnalyzeSource(R"(
        func Main() {
            for value in ..3 {}
            for value in ..=3 {}
            for value in .. {}
        }
    )");

    CHECK_EQ(std::ranges::count_if(diagnostics,
                                   [](const SemanticDiagnostic &diagnostic) {
                                       return diagnostic.message.find("has no initial value and is not iterable") !=
                                              std::string::npos;
                                   }),
             3);
}

TEST_CASE("constant ranges reject a start greater than the end") {
    const auto diagnostics = AnalyzeSource(R"(
        func Main() {
            let values: int[3] = [10, 20, 30];
            let exclusive = values[2..0];
            let inclusive = values[2..=0];
        }
    )");

    CHECK_EQ(std::ranges::count_if(diagnostics,
                                   [](const SemanticDiagnostic &diagnostic) {
                                       return diagnostic.message == "range start cannot be greater than its end";
                                   }),
             2);
}
