// Parsing of typed throws: the `throws` clause on declarations, function types and interface requirements, `throw`
// as a statement and as a diverging value, the `try` block with its `catch` arms, and the three `try` expressions with
// their precedence -- lower than every operator, and never to the right of one.

#include "Lexer/Lexer.h"
#include "Syntax/Ast/Ast.h"
#include "Syntax/Parser/Parser.h"

#include <algorithm>
#include <doctest.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

using namespace Rux;

namespace {
ParseResult ParseSource(const std::string_view source) {
    Lexer lexer(std::string(source), "throws-syntax.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "throws-syntax.rux", Target::HostArch);
    return parser.Parse();
}

ParseResult ParseClean(const std::string_view source) {
    ParseResult result = ParseSource(source);
    for (const auto &diagnostic : result.diagnostics) {
        CAPTURE(diagnostic.message);
    }
    REQUIRE_FALSE(result.HasErrors());
    return result;
}

bool HasError(const ParseResult &result, const std::string_view text) {
    return std::ranges::any_of(result.diagnostics,
                               [&](const ParserDiagnostic &diagnostic) { return diagnostic.message.contains(text); });
}

const FuncDecl &Function(const ParseResult &result, const std::string_view name) {
    for (const auto &item : result.module.items) {
        if (const auto *function = dynamic_cast<const FuncDecl *>(item.get()); function && function->name == name) {
            return *function;
        }
    }
    FAIL("missing function " << name);
    throw std::runtime_error("missing function");
}

/// The expression of statement `index` of `function`: an expression statement's, a `let`'s initializer or a
/// `return`'s value.
const Expr &StatementExpression(const FuncDecl &function, const std::size_t index) {
    REQUIRE(function.body);
    REQUIRE(index < function.body->stmts.size());
    const Stmt *statement = function.body->stmts[index].get();
    if (const auto *expression = dynamic_cast<const ExprStmt *>(statement)) {
        return *expression->expr;
    }
    if (const auto *let = dynamic_cast<const LetStmt *>(statement)) {
        return *let->init;
    }
    const auto *returned = dynamic_cast<const ReturnStmt *>(statement);
    REQUIRE(returned != nullptr);
    REQUIRE(returned->value.has_value());
    return **returned->value;
}

const FallibleTypeExpr &ThrowsClause(const std::optional<TypeExprPtr> &returnType) {
    REQUIRE(returnType.has_value());
    const auto *fallible = dynamic_cast<const FallibleTypeExpr *>(returnType->get());
    REQUIRE(fallible != nullptr);
    return *fallible;
}
} // namespace

TEST_CASE("a throws clause follows the return type or the parameter list") {
    const ParseResult result = ParseClean(R"(
        func Read(c: char) -> int ! DigitError { return 1; }
        func Close() -> !IoError {}
        func Load() -> Config ! IoError | ParseError { return Config {}; }
    )");
    const FallibleTypeExpr &read = ThrowsClause(Function(result, "Read").returnType);
    REQUIRE(read.success);
    CHECK(dynamic_cast<const NamedTypeExpr *>(read.success.get())->name == "int");
    CHECK(dynamic_cast<const NamedTypeExpr *>(read.error.get())->name == "DigitError");

    const FallibleTypeExpr &close = ThrowsClause(Function(result, "Close").returnType);
    CHECK(close.success == nullptr);

    const FallibleTypeExpr &load = ThrowsClause(Function(result, "Load").returnType);
    const auto *sum = dynamic_cast<const SumTypeExpr *>(load.error.get());
    REQUIRE(sum != nullptr);
    CHECK(sum->members.size() == 2);
}

TEST_CASE("function types and interface requirements declare throws") {
    const ParseResult result = ParseClean(R"(
        interface Source {
            func Next(&var self) -> int32 ! Closed;
            func Reset(&var self) -> !Closed;
        }
        func Apply(step: func(int32) -> int32 ! Closed, done: func() -> !Closed) {}
    )");
    const auto *source = dynamic_cast<const InterfaceDecl *>(result.module.items.front().get());
    REQUIRE(source != nullptr);
    REQUIRE(source->methods.size() == 2);
    CHECK(ThrowsClause(source->methods[0]->returnType).success != nullptr);
    CHECK(ThrowsClause(source->methods[1]->returnType).success == nullptr);

    const FuncDecl &apply = Function(result, "Apply");
    REQUIRE(apply.params.size() == 2);
    const auto *step = dynamic_cast<const FunctionTypeExpr *>(apply.params[0].type.get());
    REQUIRE(step != nullptr);
    CHECK(ThrowsClause(step->returnType).success != nullptr);
    const auto *done = dynamic_cast<const FunctionTypeExpr *>(apply.params[1].type.get());
    REQUIRE(done != nullptr);
    CHECK(ThrowsClause(done->returnType).success == nullptr);
}

TEST_CASE("throw is a statement and a diverging value, and the removed fail is reported") {
    const ParseResult result = ParseClean(R"(
        func Run(value: int?) -> int ! Fault {
            throw Fault {};
            let present = value ?? throw Fault {};
            return match present { 0 => throw Fault {}, _ => present };
        }
    )");
    const FuncDecl &run = Function(result, "Run");
    const auto *thrown = dynamic_cast<const FailStmt *>(run.body->stmts[0].get());
    REQUIRE(thrown != nullptr);
    CHECK(thrown->throwSpelling);
    const auto *coalesce = dynamic_cast<const BinaryExpr *>(&StatementExpression(run, 1));
    REQUIRE(coalesce != nullptr);
    const auto *fallback = dynamic_cast<const DivergeExpr *>(coalesce->right.get());
    REQUIRE(fallback != nullptr);
    CHECK(fallback->kind == DivergeExpr::Kind::Fail);
    CHECK(fallback->throwSpelling);
    CHECK(HasError(ParseSource("func F() -> !Fault { fail Fault {}; }"), "'fail' is not supported"));
}

TEST_CASE("postfix ! binds tightly and marks only its operand") {
    const ParseResult result = ParseClean(R"(
        func Run() -> int ! Fault {
            let sum = A()! + B()! * C()!;
            var total = 0;
            total = A()!;
            let chained = Make()!.Read()!;
            return A()!;
        }
    )");
    const FuncDecl &run = Function(result, "Run");
    const auto *sum = dynamic_cast<const BinaryExpr *>(&StatementExpression(run, 0));
    REQUIRE(sum != nullptr);
    CHECK(sum->op == TokenKind::Plus);
    const auto *left = dynamic_cast<const ErrorFormExpr *>(sum->left.get());
    REQUIRE(left != nullptr);
    CHECK(left->form == ErrorFormExpr::Form::Propagate);
    CHECK(dynamic_cast<const CallExpr *>(left->operand.get()) != nullptr);

    const auto *assignment = dynamic_cast<const AssignExpr *>(&StatementExpression(run, 2));
    REQUIRE(assignment != nullptr);
    CHECK(dynamic_cast<const ErrorFormExpr *>(assignment->value.get()) != nullptr);

    // `Make()!.Read()!` passes each call's error on in turn.
    const auto *chained = dynamic_cast<const ErrorFormExpr *>(&StatementExpression(run, 3));
    REQUIRE(chained != nullptr);
    const auto *read = dynamic_cast<const CallExpr *>(chained->operand.get());
    REQUIRE(read != nullptr);
    CHECK(dynamic_cast<const ErrorFormExpr *>(dynamic_cast<const FieldExpr &>(*read->callee).object.get()) != nullptr);
}

TEST_CASE("catch, !! and !!; handle everything to their left") {
    const ParseResult result = ParseClean(R"(
        func Run() -> int {
            let value = A() + B() catch { .Empty => 0, e: IoError if e.code > 1 => 1, _ => 2, };
            let port = Parse(text) !! Parse(other) !! 8080;
            Flush() !!;
            return value;
        }
    )");
    const FuncDecl &run = Function(result, "Run");
    const auto *value = dynamic_cast<const ErrorFormExpr *>(&StatementExpression(run, 0));
    REQUIRE(value != nullptr);
    CHECK(value->form == ErrorFormExpr::Form::Catch);
    CHECK(dynamic_cast<const BinaryExpr *>(value->operand.get()) != nullptr);
    REQUIRE(value->arms.size() == 3);
    CHECK(dynamic_cast<const GuardedPattern *>(value->arms[1].pattern.get()) != nullptr);
    CHECK(dynamic_cast<const WildcardPattern *>(value->arms[2].pattern.get()) != nullptr);

    // `a !! b !! c` is right-associative: the replacement of the first is the second handler.
    const auto *port = dynamic_cast<const ErrorFormExpr *>(&StatementExpression(run, 1));
    REQUIRE(port != nullptr);
    CHECK(port->replacesError);
    REQUIRE(port->arms.size() == 1);
    const auto *next = dynamic_cast<const ErrorFormExpr *>(port->arms.front().body.get());
    REQUIRE(next != nullptr);
    CHECK(next->replacesError);

    const auto *flush = dynamic_cast<const ErrorFormExpr *>(&StatementExpression(run, 2));
    REQUIRE(flush != nullptr);
    CHECK(flush->ignoresError);
    CHECK(flush->arms.empty());

    const ParseResult argument = ParseClean("func F() { let v = Use(C() catch { _ => 3 }); }");
    const auto *handler = dynamic_cast<const ErrorFormExpr *>(
        dynamic_cast<const CallExpr &>(StatementExpression(Function(argument, "F"), 0)).args.front().get());
    REQUIRE(handler != nullptr);
    CHECK(handler->form == ErrorFormExpr::Form::Catch);
    CHECK(dynamic_cast<const CallExpr *>(handler->operand.get()) != nullptr);
}

TEST_CASE("the removed try is reported with what replaced it, and finally is reported") {
    const auto help = [](const std::string_view source) {
        const ParseResult parsed = ParseSource(source);
        const auto found = std::ranges::find_if(parsed.diagnostics, [](const ParserDiagnostic &diagnostic) {
            return diagnostic.message == "'try' is not supported";
        });
        REQUIRE(found != parsed.diagnostics.end());
        REQUIRE(found->help.has_value());
        return *found->help;
    };
    CHECK(help("func F() { let x = try G(); }").contains("'F()!'"));
    CHECK(help("func F() { let x = 1 + try G(); }").contains("'F()!'"));
    CHECK(help("func F() { let x = try? G(); }").contains("'x !! none'"));
    CHECK(help("func F() { try { G(); } catch { _ => {} } }").contains("move its statements into a function"));
    CHECK(HasError(ParseSource("func F() { G() catch { _ => {} } finally { H(); } }"), "'finally' is not supported"));
}

TEST_CASE("throw, throws, try and finally are reserved") {
    for (const std::string_view keyword : {"throw", "throws", "try", "finally"}) {
        CAPTURE(keyword);
        CHECK(KeywordKind(keyword) != TokenKind::Ident);
    }
    CHECK(ParseSource("func F() { let try = 1; }").HasErrors());
}
