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
    if (const auto *recovery = dynamic_cast<const CatchExpr *>(expr)) {
        return "catch(" + Shape(recovery->subject.get()) + ", " + std::to_string(recovery->arms.size()) + " arms)";
    }
    if (const auto *mapped = dynamic_cast<const MappedTryExpr *>(expr)) {
        return "mapped(" + Shape(mapped->operand.get()) + ", " + mapped->binding + " => " +
               Shape(mapped->mapper.get()) + ")";
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
        static constexpr const char *kKeywords[] = {"return", "fail", "break", "continue"};
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
    CHECK_FALSE(ParsesCleanly("func F() { let none = 1; }"));
    CHECK_FALSE(ParsesCleanly("func F() { let fail = 1; }"));
    CHECK_FALSE(ParsesCleanly("func F() { let catch = 1; }"));
    // A string spelling is untouched.
    CHECK(ParsesCleanly("func F() { let text = \"none\"; }"));
}

TEST_CASE("native constructors are dot-prefixed and take one value") {
    CHECK_EQ(InitializerShape("let a = .Some(1);"), ".Some(1)");
    CHECK_EQ(InitializerShape("let a = .Success(value);"), ".Success(value)");
    CHECK_EQ(InitializerShape("let a = .Failure(error);"), ".Failure(error)");
    CHECK_EQ(InitializerShape("let a = .Some(.Some(none));"), ".Some(.Some(none))");
    CHECK_EQ(InitializerShape("let a = .Success(());"), ".Success(())");
    CHECK(Reports("func F() { let a = .Some(); }", "'.Some' takes exactly one value"));
    CHECK(Reports("func F() { let a = .Success(1, 2); }", "'.Success' takes exactly one value"));
    // Every other `.Name` keeps the existing shorthand, including `.None`.
    CHECK_EQ(InitializerShape("let a = .None;"), ".None");
    CHECK_EQ(InitializerShape("let a = .Other(1);"), "call(.Other, 1)");
}

TEST_CASE("none is a value") {
    CHECK_EQ(InitializerShape("let a = none;"), "none");
    CHECK_EQ(InitializerShape("let a = count == none;"), "(count == none)");
}

TEST_CASE("catch attaches to the nearest postfix expression") {
    CHECK_EQ(InitializerShape("let a = Read() catch { e => 0 };"), "catch(call(Read), 1 arms)");
    CHECK_EQ(InitializerShape("let a = total + Read() catch { else => 0 };"), "(total + catch(call(Read), 1 arms))");
    CHECK_EQ(InitializerShape("let a = Read() catch { else => 0 }.Length();"),
             "call(catch(call(Read), 1 arms).Length)");
    CHECK_EQ(InitializerShape("let a = (total + Read()) catch { else => 0 };"), "catch((total + call(Read)), 1 arms)");
    CHECK_EQ(InitializerShape("let a = match x { else => Read() } catch { else => 0 };"), "catch(match, 1 arms)");
}

TEST_CASE("catch arms share the match arm grammar") {
    CHECK(ParsesCleanly("func F() { let a = Read() catch { e if e.code > 0 => 1, else => 0 }; }"));
    CHECK(ParsesCleanly("func F() { let a = Read() catch { e => { Log(e); }, else => fail Other {} }; }"));
    CHECK(Reports("func F() { let a = Read() catch { e => 0, }; }", "trailing comma is not allowed in catch blocks"));
    CHECK(ReportsContaining("func F() { let a = Read() catch e => 0; }", "expected '{' to start the catch arms"));
}

TEST_CASE("an error mapping follows a tight question mark") {
    CHECK_EQ(InitializerShape("let a = Read()? else (e => Wrap(e, path));"),
             "mapped(call(Read), e => call(Wrap, e, path))");
    CHECK_EQ(InitializerShape("let a = Read()? else (_ => Fixed());"), "mapped(call(Read), _ => call(Fixed))");
    CHECK_EQ(InitializerShape("let a = Read()? else (e => Wrap(e)).Length();"),
             "call(mapped(call(Read), e => call(Wrap, e)).Length)");
    CHECK_EQ(InitializerShape("let a = Read()? else (e => fail Other {});"), "mapped(call(Read), e => fail init)");
    // Plain propagation is unchanged.
    CHECK_EQ(InitializerShape("let a = Read()?;"), "call(Read)?");
    // The parentheses re-enable struct literals inside a condition.
    CHECK(ParsesCleanly("func F() { if Read()? else (e => Wrap { cause: e }) == expected { } }"));
    CHECK(Reports("func F() { let a = Read()? else (e => 1, 2); }", "an error mapping has one body"));
}

TEST_CASE("diverging forms are accepted only as whole bodies") {
    CHECK_EQ(InitializerShape("let a = b ?? fail c ?? d;"), "(b ?? fail (c ?? d))");
    CHECK_EQ(InitializerShape("let a = b ?? return;"), "(b ?? return)");
    CHECK_EQ(InitializerShape("let a = b ?? break outer;"), "(b ?? break outer)");
    CHECK_EQ(InitializerShape("let a = b ?? continue;"), "(b ?? continue)");
    CHECK(ParsesCleanly("func F() { let a = match x { 1 => return x, else => 2 }; }"));
    CHECK(ParsesCleanly("func F() { match x { 1 => fail Other {}, else => {} } }"));
    CHECK(Reports("func F() { let a = return; }", "'return' cannot be used as a value here"));
    CHECK(Reports("func F() { G(fail x); }", "'fail' cannot be used as a value here"));
    CHECK(ReportsContaining("func F() { when Level { 1 => fail Other {}, else => {} } }",
                            "a 'when' arm body that is a statement must be written as a block"));
    // A struct literal after `fail` in an unparenthesized condition is left to the body, as for any struct literal.
    CHECK(ParsesCleanly("func F() { if (b ?? fail NotFound {}) == c { } }"));
}

TEST_CASE("fail is a statement that names its failure") {
    const ParseResult parsed = ParseSource("func F() { fail NotFound {}; fail (); }");
    REQUIRE(parsed.diagnostics.empty());
    const FuncDecl &function = OnlyFunction(parsed);
    REQUIRE_EQ(function.body->stmts.size(), 2);
    const auto *first = dynamic_cast<const FailStmt *>(function.body->stmts[0].get());
    const auto *second = dynamic_cast<const FailStmt *>(function.body->stmts[1].get());
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    CHECK_EQ(Shape(first->value.get()), "init");
    CHECK_EQ(Shape(second->value.get()), "()");
    CHECK(ReportsContaining("func F() { fail; }", "expected an expression after 'fail'"));
}

TEST_CASE("nothing postfix follows a match statement") {
    CHECK(Reports("func F() { match x { else => Read() } catch { else => 0 }; }",
                  "'catch' cannot follow a match statement"));
    CHECK(Reports("func F() { match x { else => Read() }?; }", "'?' cannot follow a match statement"));
    CHECK(Reports("func F() { match x { else => Read() } ?? 0; }", "'?"
                                                                   "?' cannot follow a match statement"));
    CHECK(ParsesCleanly("func F() { let value = match x { else => Read() } catch { else => 0 }; }"));
    CHECK(ParsesCleanly("func F() { (match x { else => Read() }) catch { else => 0 }; }"));
    CHECK(ParsesCleanly("func F() { return match x { else => Read() } catch { else => 0 }; }"));
}
