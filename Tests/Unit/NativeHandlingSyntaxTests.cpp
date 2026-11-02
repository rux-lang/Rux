// Parsing of the native handling syntax: the reserved `none`, `fail`, and `catch` keywords, native constructors,
// postfix recovery, contextual error mapping, diverging arm and fallback bodies, and the match-statement boundary.

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
    Lexer lexer(std::string(source), "native-handling-syntax.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "native-handling-syntax.rux", Target::HostArch);
    return parser.Parse();
}

/// A bracketed description of a parsed expression, naming only the node kinds these tests distinguish.
std::string Shape(const Expr *expr) {
    if (!expr) {
        return "<null>";
    }
    if (const auto *ident = dynamic_cast<const IdentExpr *>(expr)) {
        return ident->name;
    }
    if (const auto *literal = dynamic_cast<const LiteralExpr *>(expr)) {
        return literal->token.text;
    }
    if (const auto *binary = dynamic_cast<const BinaryExpr *>(expr)) {
        const std::string op = binary->op == TokenKind::QuestionQuestion ? "??"
                             : binary->op == TokenKind::Plus             ? "+"
                             : binary->op == TokenKind::Equal            ? "=="
                                                                         : "op";
        return "(" + Shape(binary->left.get()) + " " + op + " " + Shape(binary->right.get()) + ")";
    }
    if (const auto *call = dynamic_cast<const CallExpr *>(expr)) {
        std::string text = "call(" + Shape(call->callee.get());
        for (const auto &arg : call->args) {
            text += ", " + Shape(arg.get());
        }
        return text + ")";
    }
    if (const auto *field = dynamic_cast<const FieldExpr *>(expr)) {
        return Shape(field->object.get()) + "." + field->field;
    }
    if (const auto *propagate = dynamic_cast<const TryExpr *>(expr)) {
        return Shape(propagate->operand.get()) + "?";
    }
    if (const auto *construct = dynamic_cast<const NativeConstructExpr *>(expr)) {
        const char *name = construct->kind == NativeConstructExpr::Kind::Success ? ".Success"
                         : construct->kind == NativeConstructExpr::Kind::Failure ? ".Failure"
                                                                                 : ".Some";
        return std::string(name) + "(" + Shape(construct->operand.get()) + ")";
    }
    if (dynamic_cast<const NoneExpr *>(expr)) {
        return "none";
    }
    if (const auto *diverge = dynamic_cast<const DivergeExpr *>(expr)) {
        static constexpr const char *kKeywords[] = {"return", "throw", "break", "continue"};
        std::string text = kKeywords[static_cast<int>(diverge->kind)];
        if (!diverge->label.empty()) {
            text += " " + diverge->label;
        }
        return diverge->value ? text + " " + Shape(diverge->value.get()) : text;
    }
    if (const auto *tuple = dynamic_cast<const TupleExpr *>(expr)) {
        return tuple->elements.empty() ? "()" : "tuple";
    }
    if (dynamic_cast<const StructInitExpr *>(expr)) {
        return "init";
    }
    if (dynamic_cast<const MatchExpr *>(expr)) {
        return "match";
    }
    if (const auto *shorthand = dynamic_cast<const EnumShorthandExpr *>(expr)) {
        return "." + shorthand->variant;
    }
    return "?";
}

const FuncDecl &OnlyFunction(const ParseResult &parsed) {
    REQUIRE_EQ(parsed.module.items.size(), 1);
    const auto *function = dynamic_cast<const FuncDecl *>(parsed.module.items.front().get());
    REQUIRE(function != nullptr);
    return *function;
}

/// The initializer of the single `let` in `body`, which must parse without diagnostics.
std::string InitializerShape(const std::string_view body) {
    const ParseResult parsed = ParseSource("func F() { " + std::string(body) + " }");
    for (const auto &diagnostic : parsed.diagnostics) {
        FAIL_CHECK("unexpected diagnostic for '", body, "': ", diagnostic.message);
    }
    const FuncDecl &function = OnlyFunction(parsed);
    REQUIRE_EQ(function.body->stmts.size(), 1);
    const auto *let = dynamic_cast<const LetStmt *>(function.body->stmts.front().get());
    REQUIRE(let != nullptr);
    return Shape(let->init.get());
}

bool Reports(const std::string_view source, const std::string_view message) {
    const ParseResult parsed = ParseSource(source);
    for (const auto &diagnostic : parsed.diagnostics) {
        if (diagnostic.message == message) {
            return true;
        }
    }
    return false;
}

bool ReportsContaining(const std::string_view source, const std::string_view text) {
    const ParseResult parsed = ParseSource(source);
    for (const auto &diagnostic : parsed.diagnostics) {
        if (diagnostic.message.contains(text)) {
            return true;
        }
    }
    return false;
}

bool ParsesCleanly(const std::string_view source) {
    return ParseSource(source).diagnostics.empty();
}
} // namespace

TEST_CASE("none, fail, and catch are reserved keywords") {
    CHECK(KeywordKind("none") == TokenKind::NoneKeyword);
    CHECK(KeywordKind("fail") == TokenKind::FailKeyword);
    CHECK(KeywordKind("catch") == TokenKind::CatchKeyword);
    CHECK(Reports("func F() { let none = 1; }", "'none' is a reserved keyword and cannot name a binding"));
    CHECK(Reports("func F() { var none = 1; }", "'none' is a reserved keyword and cannot name a binding"));
    CHECK_FALSE(ParsesCleanly("func F() { let throw = 1; }"));
    CHECK_FALSE(ParsesCleanly("func F() { let catch = 1; }"));
    // A string spelling is untouched.
    CHECK(ParsesCleanly("func F() { let text = \"none\"; }"));
}

TEST_CASE("none is a value") {
    CHECK_EQ(InitializerShape("let a = none;"), "none");
    CHECK_EQ(InitializerShape("let a = count == none;"), "(count == none)");
}

TEST_CASE("catch arms share the match arm grammar") {
    CHECK(ParsesCleanly("func F() { let a = try Read() catch { e if e.code > 0 => 1, _ => 0 }; }"));
    CHECK(ParsesCleanly("func F() { let a = try Read() catch { e => { Log(e); }, _ => throw Other {} }; }"));
    CHECK(ParsesCleanly("func F() { let a = try Read() catch { e => 0, }; }"));
    CHECK(ParsesCleanly("func F() { let a = try Read() catch { .Missing | .Denied => 0, _ => 1 }; }"));
    CHECK(Reports("func F() { let a = try Read() catch { _ => 0, e => 1 }; }",
                  "the default arm must be the last catch arm"));
    CHECK(ReportsContaining("func F() { let a = Read() catch e => 0; }", "expected '{' to start the catch arms"));
}

TEST_CASE("diverging forms are accepted only as whole bodies") {
    CHECK_EQ(InitializerShape("let a = b ?? throw c ?? d;"), "(b ?? throw (c ?? d))");
    CHECK_EQ(InitializerShape("let a = b ?? return;"), "(b ?? return)");
    CHECK_EQ(InitializerShape("let a = b ?? break outer;"), "(b ?? break outer)");
    CHECK_EQ(InitializerShape("let a = b ?? continue;"), "(b ?? continue)");
    CHECK(ParsesCleanly("func F() { let a = match x { 1 => return x, _ => 2 }; }"));
    CHECK(ParsesCleanly("func F() { match x { 1 => throw Other {}, _ => {} } }"));
    CHECK(Reports("func F() { let a = return; }", "'return' cannot be used as a value here"));
    CHECK(Reports("func F() { G(throw x); }", "'throw' cannot be used as a value here"));
    CHECK(ReportsContaining("func F() { when Level { 1 => let x = 1, _ => {} } }",
                            "a 'when' arm body that is a statement must be written as a block"));
    // A struct literal after `throw` in an unparenthesized condition is left to the body, as for any struct literal.
    CHECK(ParsesCleanly("func F() { if (b ?? throw NotFound {}) == c { } }"));
}

TEST_CASE("throw is a statement that names its error") {
    const ParseResult parsed = ParseSource("func F() { throw NotFound {}; throw (); }");
    REQUIRE(parsed.diagnostics.empty());
    const FuncDecl &function = OnlyFunction(parsed);
    REQUIRE_EQ(function.body->stmts.size(), 2);
    const auto *first = dynamic_cast<const FailStmt *>(function.body->stmts[0].get());
    const auto *second = dynamic_cast<const FailStmt *>(function.body->stmts[1].get());
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    CHECK_EQ(Shape(first->value.get()), "init");
    CHECK_EQ(Shape(second->value.get()), "()");
    CHECK(ReportsContaining("func F() { throw; }", "expected an expression after 'throw'"));
}
