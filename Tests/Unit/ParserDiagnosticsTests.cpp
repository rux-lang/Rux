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

ParseResult ParseSource(const std::string_view source, const Target::Arch arch = Target::HostArch) {
    Lexer lexer(std::string(source), "parser-diagnostics.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "parser-diagnostics.rux", arch);
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

std::string DiagnosticMessages(const ParseResult &result) {
    std::string messages;
    for (const auto &diagnostic : result.diagnostics) {
        if (!messages.empty()) {
            messages += " | ";
        }
        messages += diagnostic.message;
    }
    return messages;
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
        CAPTURE(DiagnosticMessages(parsed));
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
        CAPTURE(DiagnosticMessages(parsed));
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

TEST_CASE("generic declarations retain single and multiple interface bounds in the AST") {
    const auto parsed = ParseSource(R"(
        func Render<T: Display + Core::Debug, Context>(value: T, context: Context);
        struct Table<Key: Hash + Equal, Value> { key: Key; value: Value; }
        enum Outcome<Value, Error: Display> { Ok(Value), Fail(Error) }
    )");

    CAPTURE(DiagnosticMessages(parsed));
    REQUIRE_FALSE(parsed.HasErrors());
    REQUIRE_EQ(parsed.module.items.size(), 3);

    const auto *function = dynamic_cast<const FuncDecl *>(parsed.module.items[0].get());
    REQUIRE(function != nullptr);
    REQUIRE_EQ(function->typeParams.size(), 2);
    CHECK_EQ(function->typeParams[0].name, "T");
    CHECK_EQ(function->typeParams[1].name, "Context");
    REQUIRE_EQ(function->typeParams[0].bounds.size(), 2);
    CHECK(dynamic_cast<const NamedTypeExpr *>(function->typeParams[0].bounds[0].get()) != nullptr);
    CHECK(dynamic_cast<const PathTypeExpr *>(function->typeParams[0].bounds[1].get()) != nullptr);
    CHECK(function->typeParams[1].bounds.empty());

    const auto *structure = dynamic_cast<const StructDecl *>(parsed.module.items[1].get());
    REQUIRE(structure != nullptr);
    REQUIRE_EQ(structure->typeParams.size(), 2);
    CHECK_EQ(structure->typeParams[0].name, "Key");
    CHECK_EQ(structure->typeParams[0].bounds.size(), 2);
    CHECK(structure->typeParams[1].bounds.empty());

    const auto *enumeration = dynamic_cast<const EnumDecl *>(parsed.module.items[2].get());
    REQUIRE(enumeration != nullptr);
    REQUIRE_EQ(enumeration->typeParams.size(), 2);
    CHECK(enumeration->typeParams[0].bounds.empty());
    CHECK_EQ(enumeration->typeParams[1].name, "Error");
    CHECK_EQ(enumeration->typeParams[1].bounds.size(), 1);
}

TEST_CASE("generic bound diagnostics identify missing names and separators") {
    struct Case {
        std::string_view source;
        std::string_view expected;
    };

    constexpr Case cases[] = {
        {"func Missing<T:>(value: T);", "expected an interface bound after ':' before '>'"},
        {"struct Trailing<T: Display +> {}", "expected an interface bound after '+' before '>'"},
        {"enum Adjacent<T: Display Debug> { Value(T) }", "expected '+' between interface bounds before 'Debug'"},
        {"func Invalid<T: *Value>();", "expected an interface name in the type parameter bound before '*'"},
    };

    for (const Case &testCase : cases) {
        CAPTURE(testCase.source);
        const ParseResult parsed = ParseSource(testCase.source);
        CAPTURE(DiagnosticMessages(parsed));
        CHECK_EQ(parsed.diagnostics.size(), 1);
        CHECK(FindDiagnostic(parsed, testCase.expected) != nullptr);
    }
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

TEST_CASE("expression diagnostics name their owning operator and delimiter") {
    struct Case {
        std::string_view source;
        std::string_view expected;
    };

    constexpr Case cases[] = {
        {"func F() { let value = ; }", "expected an expression after '=' in the binding before ';'"},
        {"func F() { let value = 1 + ; }", "expected an expression after '+' before ';'"},
        {"func F() { let value = !; }", "expected an expression after unary '!' before ';'"},
        {"func F() { let value = flag ? : 0; }",
         "expected an expression after '?' in the conditional expression before ':'"},
        {"func F() { let value = flag ? 1 : ; }",
         "expected an expression after ':' in the conditional expression before ';'"},
        {"func F() { let value = Call(1 2); }", "expected ',' between arguments before '2'"},
        {"func F() { let value = Call(1; }", "expected ')' to close the argument list before ';'"},
        {"func F() { let value = items[]; }", "expected an expression after '[' in the index expression before ']'"},
        {"func F() { let value = [1 2]; }", "expected ',' between slice elements before '2'"},
        {"func F() { let value = Pair { first: }; }",
         "expected an expression after ':' in the initializer field before '}'"},
    };

    for (const auto &testCase : cases) {
        CAPTURE(testCase.source);
        const auto parsed = ParseSource(testCase.source);
        CAPTURE(DiagnosticMessages(parsed));
        CHECK(FindDiagnostic(parsed, testCase.expected) != nullptr);
    }
}

TEST_CASE("a binary operator written where an expression starts reports its own misuse") {
    // Each of these leads with a token that only joins two operands. The
    // parser used to build the operator node with nothing on its left, which
    // left an expression no later stage could type and no diagnostic to say
    // why, so the report each case owns is what keeps the tree well formed.
    struct Case {
        std::string_view source;
        std::string_view expected;
    };

    constexpr Case cases[] = {
        {"func F() { let value = Call(&count); }",
         "'&' does not take an address; write '@' to take the address of a value"},
        {"func F() { let value = &count; }", "'&' does not take an address; write '@' to take the address of a value"},
        {"func F() { let value = Call(|count); }", "expected an expression after '(' in the argument list before '|'"},
        {"func F() { let value = ^count; }", "expected an expression after '=' in the binding before '^'"},
        {"func F() { let value = Call(as int); }", "expected an expression after '(' in the argument list before 'as'"},
    };

    for (const auto &testCase : cases) {
        CAPTURE(testCase.source);
        const auto parsed = ParseSource(testCase.source);
        CAPTURE(DiagnosticMessages(parsed));
        CHECK(FindDiagnostic(parsed, testCase.expected) != nullptr);
    }
}

TEST_CASE("the C address-of spelling reports the rewrite once and recovers as '@'") {
    // Recovering as address-of is what stops one rejected sigil from cascading
    // into a missing argument and an arity error on the call that holds it.
    const auto parsed = ParseSource("func F() { let flag = Take(&bytes); }");
    CAPTURE(DiagnosticMessages(parsed));
    REQUIRE_EQ(parsed.diagnostics.size(), 1);
    CHECK_EQ(parsed.diagnostics[0].message, "'&' does not take an address; write '@' to take the address of a value");
    CHECK_EQ(parsed.diagnostics[0].location.column, 28);
    CHECK_FALSE(HasDiagnosticContaining(parsed, "expected an expression"));
}

TEST_CASE("'&' between two operands stays the bitwise operator") {
    const auto parsed = ParseSource("func F() { let value = left & right; let masked = left&right; }");
    CAPTURE(DiagnosticMessages(parsed));
    CHECK_FALSE(parsed.HasErrors());
}

TEST_CASE("statement diagnostics identify the condition, iterable, body, and terminator") {
    struct Case {
        std::string_view source;
        std::string_view expected;
    };

    constexpr Case cases[] = {
        {"func F() { if {} }", "expected an expression after 'if' before '{'"},
        {"func F() { while {} }", "expected an expression after 'while' before '{'"},
        {"func F() { do {} while ; }", "expected an expression after 'while' in the 'do' statement before ';'"},
        {"func F() { for item in {} }", "expected an expression after 'in' in the 'for' statement before '{'"},
        {"func F() { loop ; }", "expected '{' to start the 'loop' body before ';'"},
        {"func F() { match {} }", "expected an expression after 'match' before '{'"},
        {"func F() { break }", "expected ';' after the 'break' statement before '}'"},
        {"func F() { continue }", "expected ';' after the 'continue' statement before '}'"},
        {"func F() { return 1 }", "expected ';' after the 'return' statement before '}'"},
        {"func F() { let value: int }", "expected ';' after the binding declaration before '}'"},
    };

    for (const auto &testCase : cases) {
        CAPTURE(testCase.source);
        const auto parsed = ParseSource(testCase.source);
        CAPTURE(DiagnosticMessages(parsed));
        CHECK(FindDiagnostic(parsed, testCase.expected) != nullptr);
    }
}

TEST_CASE("pattern diagnostics cover destructuring, ranges, guards, and match arms") {
    struct Case {
        std::string_view source;
        std::string_view expected;
    };

    constexpr Case cases[] = {
        {"func F() { let (first second) = value; }", "expected ',' between tuple pattern elements before 'second'"},
        {"func F() { let Point { x: first y: second } = value; }",
         "expected ',' between structure pattern fields before 'y'"},
        {"func F() { match value { .Some(first second) => 1 } }",
         "expected ',' between variant pattern elements before 'second'"},
        {"func F() { match value { 1.. => 1 } }", "expected a range pattern end after '..' before '=>'"},
        {"func F() { match value { item if => 1 } }",
         "expected an expression after 'if' in the pattern guard before '=>'"},
        {"func F() { match value { => 1 } }", "expected a pattern at the start of the match arm before '=>'"},
    };

    for (const auto &testCase : cases) {
        CAPTURE(testCase.source);
        const auto parsed = ParseSource(testCase.source);
        CHECK(FindDiagnostic(parsed, testCase.expected) != nullptr);
    }
}

TEST_CASE("match recovery reaches arms after malformed patterns and bodies") {
    const auto parsed = ParseSource(R"(
func F(value: int) {
    match value {
        => 0,
        1 => ,
        2 => 2
    }
}
)");

    CHECK(FindDiagnostic(parsed, "expected a pattern at the start of the match arm before '=>'") != nullptr);
    CHECK(FindDiagnostic(parsed, "expected an expression after '=>' in the match arm before ','") != nullptr);
    REQUIRE_EQ(parsed.module.items.size(), 1);
    const auto *function = dynamic_cast<const FuncDecl *>(parsed.module.items[0].get());
    REQUIRE(function != nullptr);
    REQUIRE(function->body != nullptr);
    const auto *match = dynamic_cast<const MatchStmt *>(function->body->stmts[0].get());
    REQUIRE(match != nullptr);
    CHECK_EQ(match->arms.size(), 3);
}

TEST_CASE("every pattern family retains its accepted syntax") {
    const auto parsed = ParseSource(R"(
func F(value: int) {
    match value {
        0 => 0,
        -1 => 1,
        .Some(item) => 2,
        Event::Named { value: inner } => 3,
        Point { x: first, y: second } => 4,
        (first, second) => 5,
        1..=3 => 6,
        item if item > 0 => 7,
        else => 8
    }
}
)");

    CAPTURE(DiagnosticMessages(parsed));
    CHECK_FALSE(parsed.HasErrors());
    REQUIRE_EQ(parsed.module.items.size(), 1);
    const auto *function = dynamic_cast<const FuncDecl *>(parsed.module.items[0].get());
    REQUIRE(function != nullptr);
    REQUIRE(function->body != nullptr);
    const auto *match = dynamic_cast<const MatchStmt *>(function->body->stmts[0].get());
    REQUIRE(match != nullptr);
    CHECK_EQ(match->arms.size(), 9);
}

TEST_CASE("assembly diagnostics cover malformed instruction and operand forms on both targets") {
    struct Case {
        Target::Arch arch;
        std::string_view source;
        std::string_view expected;
    };

    constexpr Case cases[] = {
        {Target::Arch::X86_64, "asm func F() { 1 }", "expected an assembly instruction mnemonic or label before '1'"},
        {Target::Arch::X86_64, "asm func F() { mov rax, }",
         "expected an operand after ',' in the 'mov' instruction before '}'"},
        {Target::Arch::X86_64, "asm func F() { mov rax, [rbp +] }",
         "expected a memory operand term after '+' before ']'"},
        {Target::Arch::X86_64, "asm func F() { lea rax, [rbp + rax*] }",
         "expected an integer scale after '*' in the memory operand before ']'"},
        {Target::Arch::AArch64, "asm func F() { add x0, x1, x2, lsl }",
         "expected an integer shift amount after 'lsl' before '}'"},
        {Target::Arch::AArch64, "asm func F() { ldr x0, [x1 }", "expected ']' to close the memory operand before '}'"},
    };

    for (const auto &testCase : cases) {
        CAPTURE(testCase.source);
        const auto parsed = ParseSource(testCase.source, testCase.arch);
        CAPTURE(DiagnosticMessages(parsed));
        CHECK(FindDiagnostic(parsed, testCase.expected) != nullptr);
    }
}

TEST_CASE("a when match arm loop always consumes something") {
    // A statement written as a bare arm body parses no expression and so consumed nothing, and the arm loop ran
    // forever building empty arms until the process ran out of memory. Each malformed arm is now reported once and
    // the match ends.
    const auto parsed = ParseSource(R"(
        func Select() -> int32 {
            when #target.os {
                .Windows => return 1i32;
                else => return 2i32;
            }
        }
    )");

    REQUIRE(parsed.HasErrors());
    CAPTURE(DiagnosticMessages(parsed));
    const std::string expected =
        "a 'when' arm body that is a statement must be written as a block, as in '.Windows => { return 1; }'";
    std::size_t reported = 0;
    for (const auto &diagnostic : parsed.diagnostics) {
        if (diagnostic.message == expected) {
            ++reported;
        }
    }
    // One per malformed arm: recovery takes the statement's terminator with it, so the next arm starts cleanly
    // instead of the leftover ';' being read as another arm.
    CHECK_EQ(reported, 2);
    CHECK_EQ(parsed.diagnostics.size(), 2);
}

TEST_CASE("a when match arm accepts an expression body and a block body") {
    const auto parsed = ParseSource(R"(
        func Select() -> int32 {
            var result = 0i32;
            when #target.os {
                .Windows => result = 1i32,
                .Linux => { result = 2i32; }
                else => result = 3i32
            }
            return result;
        }
    )");

    CAPTURE(DiagnosticMessages(parsed));
    CHECK_FALSE(parsed.HasErrors());
}
