// Declaration and type parser diagnostic ownership and recovery tests.

#include "Lexer/Lexer.h"
#include "Syntax/Parser/Parser.h"

#include <doctest.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Rux;

namespace {

ParseResult ParseSource(const std::string_view source) {
    Lexer lexer(std::string(source), "parser-diagnostics.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "parser-diagnostics.rux");
    return parser.Parse();
}

const Diagnostic *FindDiagnostic(const ParseResult &result, const std::string_view message) {
    for (const auto &diagnostic : result.diagnostics) {
        if (diagnostic.message == message) {
            return &diagnostic;
        }
    }
    return nullptr;
}

bool HasDiagnosticContaining(const ParseResult &result, const std::string_view text) {
    for (const auto &diagnostic : result.diagnostics) {
        if (diagnostic.message.contains(text)) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("declaration diagnostics identify the rejected starter and expected declaration role") {
    struct Case {
        std::string_view source;
        std::string_view expected;
    };

    constexpr Case cases[] = {
        {";", "expected a declaration before ';'"},
        {"pub ;", "expected a declaration after 'pub' before ';'"},
        {"func () {}", "expected a function name after 'func' before '('"},
        {"struct {}", "expected a structure name after 'struct' before '{'"},
        {"enum {}", "expected an enum name after 'enum' before '{'"},
        {"union {}", "expected a union name after 'union' before '{'"},
        {"interface {}", "expected an interface name after 'interface' before '{'"},
        {"module {}", "expected a module name after 'module' before '{'"},
        {"type = int;", "expected a type alias name after 'type' before '='"},
        {"const = 1;", "expected a constant name after 'const' before '='"},
        {"extend {}", "expected a type before '{'"},
        {"import ;", "expected a module path after 'import' before ';'"},
        {"extern ;", "expected a function, variable name, or '{' after 'extern' before ';'"},
        {"intrinsic ;", "expected a '#'-prefixed value or 'func' after 'intrinsic' before ';'"},
    };

    for (const auto &testCase : cases) {
        CAPTURE(testCase.source);
        const auto parsed = ParseSource(testCase.source);
        CHECK(FindDiagnostic(parsed, testCase.expected) != nullptr);
    }
}

TEST_CASE("attribute diagnostics describe valid call syntax and removed metadata") {
    const auto missingName = ParseSource("#) func Good();");
    const auto *nameDiagnostic = FindDiagnostic(missingName, "expected an attribute name after '#' before ')'");
    REQUIRE(nameDiagnostic != nullptr);
    REQUIRE(nameDiagnostic->help.has_value());
    CHECK(nameDiagnostic->help->contains("#Name(...)"));
    REQUIRE_EQ(missingName.module.items.size(), 1);

    const auto abi = ParseSource("#Abi(C) func Good();");
    CHECK(FindDiagnostic(abi, "expected '.' before the ABI name before 'C'") != nullptr);

    const auto link = ParseSource("#Link() extern func Good();");
    CHECK(FindDiagnostic(link, "expected a library string or compile-time string constant in '#Link' before ')'") !=
          nullptr);

    const auto metadata = ParseSource("#{ Target = .Windows } func Good();");
    CHECK(HasDiagnosticContaining(metadata, "metadata blocks '#{...}' are unsupported"));
    REQUIRE_EQ(metadata.module.items.size(), 1);
}

TEST_CASE("declaration delimiters and list separators identify their grammar roles") {
    struct Case {
        std::string_view source;
        std::string_view expected;
    };

    constexpr Case cases[] = {
        {"func F;", "expected '(' after the function name before ';'"},
        {"func F(value int);", "expected ':' after the parameter name before 'int'"},
        {"func F(first: int second: bool);", "expected ',' between parameters before 'second'"},
        {"struct Box<T U> {}", "expected ',' between type parameters before 'U'"},
        {"enum Choice { First Second }", "expected ',' between enum variants before 'Second'"},
        {"union Bits { low: int high: int }", "expected ',' between union fields before 'high'"},
        {"import Core::{First Second};", "expected ',' between imported names before 'Second'"},
        {"type Pair = Pair<int bool>;", "expected ',' between type arguments before 'bool'"},
        {"type Callback = func(int bool);", "expected ',' between function type parameters before 'bool'"},
        {"type Buffer = int[4;", "expected ']' to close the array type before ';'"},
        {"extern func F(value: int) -> ;", "expected a type before ';'"},
        {"extern { value int; }", "expected ':' after the external variable name before 'int'"},
    };

    for (const auto &testCase : cases) {
        CAPTURE(testCase.source);
        const auto parsed = ParseSource(testCase.source);
        CHECK(FindDiagnostic(parsed, testCase.expected) != nullptr);
    }
}

TEST_CASE("type diagnostics carry declaration-specific corrective help") {
    const auto field = ParseSource("struct Broken { value: }");
    const auto *fieldDiagnostic = FindDiagnostic(field, "expected a type before '}'");
    REQUIRE(fieldDiagnostic != nullptr);
    CHECK(fieldDiagnostic->help == "add the field type after ':'");

    const auto parameter = ParseSource("func Broken(value: );");
    const auto *parameterDiagnostic = FindDiagnostic(parameter, "expected a type before ')'");
    REQUIRE(parameterDiagnostic != nullptr);
    CHECK(parameterDiagnostic->help == "add the parameter type after ':'");

    const auto returnType = ParseSource("func Broken() -> ;");
    const auto *returnDiagnostic = FindDiagnostic(returnType, "expected a type before ';'");
    REQUIRE(returnDiagnostic != nullptr);
    CHECK(returnDiagnostic->help == "add the function return type after '->'");

    const auto alias = ParseSource("type Broken = ;");
    const auto *aliasDiagnostic = FindDiagnostic(alias, "expected a type before ';'");
    REQUIRE(aliasDiagnostic != nullptr);
    CHECK(aliasDiagnostic->help == "add the aliased type after '='");
}

TEST_CASE("nested generics and function types retain valid declaration syntax") {
    const auto parsed = ParseSource("type Nested = Outer<Inner<int> >; type Callback = func(int, *var byte) -> bool;");
    CHECK_FALSE(parsed.HasErrors());
    CHECK_EQ(parsed.module.items.size(), 2);
}

TEST_CASE("declaration recovery reaches fields and declarations after a malformed item") {
    const auto parsed = ParseSource(R"(
struct Recovered {
    first: int
    second: bool;
}
type Broken = ;
struct MissingBody
func AfterMissingBody();
type MissingParentheses = func int;
func Good();
)");

    CHECK(FindDiagnostic(parsed, "expected ';' after the structure field before 'second'") != nullptr);
    CHECK(FindDiagnostic(parsed, "expected a type before ';'") != nullptr);
    CHECK(FindDiagnostic(parsed, "expected '{' to start the structure body before 'func'") != nullptr);
    CHECK(FindDiagnostic(parsed, "expected '(' after 'func' before 'int'") != nullptr);
    REQUIRE_EQ(parsed.module.items.size(), 6);
    const auto *structure = dynamic_cast<const StructDecl *>(parsed.module.items[0].get());
    REQUIRE(structure != nullptr);
    CHECK_EQ(structure->fields.size(), 2);
    const auto *recoveredFunction = dynamic_cast<const FuncDecl *>(parsed.module.items[3].get());
    REQUIRE(recoveredFunction != nullptr);
    CHECK_EQ(recoveredFunction->name, "AfterMissingBody");
    const auto *function = dynamic_cast<const FuncDecl *>(parsed.module.items[5].get());
    REQUIRE(function != nullptr);
    CHECK_EQ(function->name, "Good");
}
