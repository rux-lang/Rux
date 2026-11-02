// Parsing of the native sum, fallible, optional, and unit spellings: precedence, grouping, suffix order, generic
// arguments, the tight optional suffix, and the diagnostics for spellings the grammar requires to be grouped.

#include "Formatter/Formatter.h"
#include "Lexer/Lexer.h"
#include "Syntax/Ast/Ast.h"
#include "Syntax/Parser/Parser.h"

#include <doctest.h>
#include <string>
#include <string_view>
#include <utility>

using namespace Rux;

namespace {
ParseResult ParseSource(const std::string_view source) {
    Lexer lexer(std::string(source), "native-type-syntax.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "native-type-syntax.rux", Target::HostArch);
    return parser.Parse();
}

/// A fully bracketed description of a parsed type, so a test reads the tree the parser built rather than a spelling
/// that could hide a precedence mistake.
std::string Shape(const TypeExpr *type) {
    if (!type) {
        return "<null>";
    }
    const auto list = [](const auto &children) {
        std::string text;
        for (std::size_t index = 0; index < children.size(); ++index) {
            text += (index ? ", " : "") + Shape(children[index].get());
        }
        return text;
    };
    if (const auto *named = dynamic_cast<const NamedTypeExpr *>(type)) {
        return named->typeArgs.empty() ? named->name : named->name + "<" + list(named->typeArgs) + ">";
    }
    if (const auto *sum = dynamic_cast<const SumTypeExpr *>(type)) {
        return "sum(" + list(sum->members) + ")";
    }
    if (const auto *optional = dynamic_cast<const OptionalTypeExpr *>(type)) {
        return "opt(" + Shape(optional->payload.get()) + ")";
    }
    if (const auto *fallible = dynamic_cast<const FallibleTypeExpr *>(type)) {
        return "fal(" + (fallible->success ? Shape(fallible->success.get()) : std::string("unit")) + ", " +
               Shape(fallible->error.get()) + ")";
    }
    if (const auto *pointer = dynamic_cast<const PointerTypeExpr *>(type)) {
        return "ptr(" + Shape(pointer->pointee.get()) + ")";
    }
    if (const auto *reference = dynamic_cast<const ReferenceTypeExpr *>(type)) {
        return "ref(" + Shape(reference->pointee.get()) + ")";
    }
    if (const auto *slice = dynamic_cast<const SliceTypeExpr *>(type)) {
        return std::string(slice->elementMut ? "varslice(" : "slice(") + Shape(slice->element.get()) + ")";
    }
    if (const auto *array = dynamic_cast<const ArrayTypeExpr *>(type)) {
        return "array(" + Shape(array->element.get()) + ")";
    }
    if (const auto *tuple = dynamic_cast<const TupleTypeExpr *>(type)) {
        return "tuple(" + list(tuple->elements) + ")";
    }
    if (const auto *function = dynamic_cast<const FunctionTypeExpr *>(type)) {
        return "func(" + list(function->params) + ")->" +
               (function->returnType ? Shape(function->returnType->get()) : std::string("void"));
    }
    return "?";
}

/// The shape of `spelling` parsed as the type of a parameter, with no diagnostics allowed.
std::string ParameterShape(const std::string_view spelling) {
    const ParseResult parsed = ParseSource("func F(value: " + std::string(spelling) + ") {}");
    for (const auto &diagnostic : parsed.diagnostics) {
        FAIL_CHECK("unexpected diagnostic for '", spelling, "': ", diagnostic.message);
    }
    REQUIRE_EQ(parsed.module.items.size(), 1);
    const auto *function = dynamic_cast<const FuncDecl *>(parsed.module.items.front().get());
    REQUIRE(function != nullptr);
    REQUIRE_EQ(function->params.size(), 1);
    return Shape(function->params.front().type.get());
}

/// Whether parsing `source` reports a diagnostic with exactly `message`.
bool Reports(const std::string_view source, const std::string_view message) {
    const ParseResult parsed = ParseSource(source);
    for (const auto &diagnostic : parsed.diagnostics) {
        if (diagnostic.message == message) {
            return true;
        }
    }
    return false;
}

/// The single statement of `F`'s body in `source`, which must parse cleanly.
const Stmt &OnlyStatement(const ParseResult &parsed) {
    REQUIRE(parsed.diagnostics.empty());
    const auto *function = dynamic_cast<const FuncDecl *>(parsed.module.items.front().get());
    REQUIRE(function != nullptr);
    REQUIRE_EQ(function->body->stmts.size(), 1);
    return *function->body->stmts.front();
}
} // namespace

TEST_CASE("optional suffixes keep every level and apply left to right") {
    CHECK_EQ(ParameterShape("int32?"), "opt(int32)");
    CHECK_EQ(ParameterShape("int32??"), "opt(opt(int32))");
    CHECK_EQ(ParameterShape("int32???"), "opt(opt(opt(int32)))");
    CHECK_EQ(ParameterShape("int32?[..]"), "slice(opt(int32))");
    CHECK_EQ(ParameterShape("int32[..]?"), "opt(slice(int32))");
    CHECK_EQ(ParameterShape("T?[4]"), "array(opt(T))");
    CHECK_EQ(ParameterShape("T[4]?"), "opt(array(T))");
    CHECK_EQ(ParameterShape("(var char8[..])?"), "opt(varslice(char8))");
    CHECK(Reports("func F(value: var char8[..]?) {}", "'var' in a type qualifies only a slice's elements"));
}

TEST_CASE("an optional sum member must be grouped") {
    CHECK_EQ(ParameterShape("A | (B?)"), "sum(A, opt(B))");
    CHECK_EQ(ParameterShape("(A | B)?"), "opt(sum(A, B))");
    CHECK(Reports("func F(value: A | B?) {}", "an optional sum member must be grouped"));
    CHECK(Reports("func F(value: A? | B) {}", "an optional sum member must be grouped"));
}

TEST_CASE("function return types stay complete and function members are grouped") {
    CHECK_EQ(ParameterShape("func() -> A | B"), "func()->sum(A, B)");
    CHECK_EQ(ParameterShape("func() -> A throws E"), "func()->fal(A, E)");
    CHECK_EQ(ParameterShape("(func() -> A) | B"), "sum(func()->A, B)");
    CHECK(Reports("func F(value: B | func() -> A) {}", "a function type in a sum must be grouped"));
}

TEST_CASE("empty parentheses are the unit value") {
    const ParseResult parsed = ParseSource("func F() { let unit: () = (); }");
    const auto *let = dynamic_cast<const LetStmt *>(&OnlyStatement(parsed));
    REQUIRE(let != nullptr);
    REQUIRE(let->type.has_value());
    CHECK_EQ(Shape(let->type->get()), "tuple()");
    const auto *unit = dynamic_cast<const TupleExpr *>(let->init.get());
    REQUIRE(unit != nullptr);
    CHECK(unit->elements.empty());
}

TEST_CASE("formatting leaves native type spellings unchanged") {
    const std::string source = "func F(a: A | B, b: int32??, c: ! E, d: (A ! E) ! F, e: *(A | B), f: Box<T?>) {}\n";
    const auto formatted = Formatting::Format(source);
    CHECK_FALSE(formatted.changed);
    CHECK_EQ(formatted.text, source);
}
