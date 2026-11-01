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
    CHECK(fallible->throwsClause);
    return *fallible;
}
} // namespace

TEST_CASE("a throws clause follows the return type or the parameter list") {
    const ParseResult result = ParseClean(R"(
        func Read(c: char) -> int throws DigitError { return 1; }
        func Close() throws IoError {}
        func Load() -> Config throws IoError | ParseError { return Config {}; }
        func Older() -> int ! DigitError { return 1; }
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

    const auto &older = Function(result, "Older").returnType;
    const auto *fallible = dynamic_cast<const FallibleTypeExpr *>(older->get());
    REQUIRE(fallible != nullptr);
    CHECK_FALSE(fallible->throwsClause);
}

TEST_CASE("function types and interface requirements declare throws") {
    const ParseResult result = ParseClean(R"(
        interface Source {
            func Next(self: &var Self) -> int32 throws Closed;
            func Reset(self: &var Self) throws Closed;
        }
        func Apply(step: func(int32) -> int32 throws Closed, done: func() throws Closed) {}
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

TEST_CASE("a fallible return type and a throws clause are not combined") {
    const ParseResult result = ParseSource("func Both() -> int ! IoError throws ParseError { return 1; }");
    CHECK(HasError(result, "a function that returns 'T ! E' cannot also declare 'throws'"));
}

TEST_CASE("throw is a statement and a diverging value, and fail is its older spelling") {
    const ParseResult result = ParseClean(R"(
        func Run(value: int?) -> int throws Fault {
            throw Fault {};
            fail Fault {};
            let present = value ?? throw Fault {};
            return match present { 0 => throw Fault {}, _ => present };
        }
    )");
    const FuncDecl &run = Function(result, "Run");
    const auto *thrown = dynamic_cast<const FailStmt *>(run.body->stmts[0].get());
    REQUIRE(thrown != nullptr);
    CHECK(thrown->throwSpelling);
    const auto *failed = dynamic_cast<const FailStmt *>(run.body->stmts[1].get());
    REQUIRE(failed != nullptr);
    CHECK_FALSE(failed->throwSpelling);
    const auto *coalesce = dynamic_cast<const BinaryExpr *>(&StatementExpression(run, 2));
    REQUIRE(coalesce != nullptr);
    const auto *fallback = dynamic_cast<const DivergeExpr *>(coalesce->right.get());
    REQUIRE(fallback != nullptr);
    CHECK(fallback->kind == DivergeExpr::Kind::Fail);
    CHECK(fallback->throwSpelling);
}

TEST_CASE("try covers the whole expression to its right") {
    const ParseResult result = ParseClean(R"(
        func Run() -> int throws Fault {
            let sum = try A() + B() * C();
            var total = 0;
            total = try A();
            total += try B();
            let maybe = try? A() ?? 0;
            Use(try A(), try? B());
            return try A();
        }
    )");
    const FuncDecl &run = Function(result, "Run");
    const auto *sum = dynamic_cast<const TryMarkExpr *>(&StatementExpression(run, 0));
    REQUIRE(sum != nullptr);
    CHECK(sum->form == TryMarkExpr::Form::Propagate);
    const auto *addition = dynamic_cast<const BinaryExpr *>(sum->operand.get());
    REQUIRE(addition != nullptr);
    CHECK(addition->op == TokenKind::Plus);

    for (const std::size_t index : {std::size_t{2}, std::size_t{3}}) {
        const auto *assignment = dynamic_cast<const AssignExpr *>(&StatementExpression(run, index));
        REQUIRE(assignment != nullptr);
        CHECK(dynamic_cast<const TryMarkExpr *>(assignment->value.get()) != nullptr);
    }

    // `try?` also takes the `??` after it.
    const auto *maybe = dynamic_cast<const TryMarkExpr *>(&StatementExpression(run, 4));
    REQUIRE(maybe != nullptr);
    CHECK(maybe->form == TryMarkExpr::Form::Optional);
    const auto *coalesce = dynamic_cast<const BinaryExpr *>(maybe->operand.get());
    REQUIRE(coalesce != nullptr);
    CHECK(coalesce->op == TokenKind::QuestionQuestion);

    const auto *call = dynamic_cast<const CallExpr *>(&StatementExpression(run, 5));
    REQUIRE(call != nullptr);
    REQUIRE(call->args.size() == 2);
    CHECK(dynamic_cast<const TryMarkExpr *>(call->args[0].get())->form == TryMarkExpr::Form::Propagate);
    CHECK(dynamic_cast<const TryMarkExpr *>(call->args[1].get())->form == TryMarkExpr::Form::Optional);
}

TEST_CASE("catch after a try operand takes the arms, and inside nested expressions is postfix again") {
    const ParseResult result = ParseClean(R"(
        func Run() -> int {
            let value = try A() + B() catch { .Empty => 0, e: IoError if e.code > 1 => 1, _ => 2, };
            let older = try Use(C() catch { _ => 3 });
            return value;
        }
    )");
    const FuncDecl &run = Function(result, "Run");
    const auto *value = dynamic_cast<const TryMarkExpr *>(&StatementExpression(run, 0));
    REQUIRE(value != nullptr);
    CHECK(value->form == TryMarkExpr::Form::Catch);
    CHECK(dynamic_cast<const BinaryExpr *>(value->operand.get()) != nullptr);
    REQUIRE(value->arms.size() == 3);
    CHECK(dynamic_cast<const GuardedPattern *>(value->arms[1].pattern.get()) != nullptr);
    CHECK(dynamic_cast<const WildcardPattern *>(value->arms[2].pattern.get()) != nullptr);

    const auto *older = dynamic_cast<const TryMarkExpr *>(&StatementExpression(run, 1));
    REQUIRE(older != nullptr);
    CHECK(older->form == TryMarkExpr::Form::Propagate);
    const auto *use = dynamic_cast<const CallExpr *>(older->operand.get());
    REQUIRE(use != nullptr);
    CHECK(dynamic_cast<const CatchExpr *>(use->args.front().get()) != nullptr);
}

TEST_CASE("a try block holds its statements and its catch arms") {
    const ParseResult result = ParseClean(R"(
        func Run() {
            try {
                let value = Read();
                Use(value);
            } catch {
                .NotADigit(c) => Report(c),
                .Blank | .Empty => {}
                e => throw e,
            }
        }
    )");
    const FuncDecl &run = Function(result, "Run");
    const auto *handler = dynamic_cast<const TryStmt *>(run.body->stmts.front().get());
    REQUIRE(handler != nullptr);
    REQUIRE(handler->body);
    CHECK(handler->body->stmts.size() == 2);
    REQUIRE(handler->arms.size() == 3);
    CHECK(dynamic_cast<const AlternativePattern *>(handler->arms[1].pattern.get()) != nullptr);
    CHECK(dynamic_cast<const DivergeExpr *>(handler->arms[2].body.get()) != nullptr);
}

TEST_CASE("misplaced try, try? with arms, a try block without arms and finally are reported") {
    CHECK(
        HasError(ParseSource("func F() { let x = 1 + try G(); }"), "'try' cannot appear to the right of an operator"));
    CHECK(HasError(ParseSource("func F() { let x = !try G(); }"), "'try' cannot appear to the right of an operator"));
    CHECK(HasError(ParseSource("func F() { let x = try? G() catch { _ => 1 }; }"), "'try?' takes no 'catch' arms"));
    CHECK(HasError(ParseSource("func F() { try { G(); } }"), "a 'try' block needs 'catch' arms"));
    const ParseResult finallyAfterCatch = ParseSource("func F() { try { G(); } catch { _ => {} } finally { H(); } }");
    CHECK(HasError(finallyAfterCatch, "'finally' is not supported"));
    const ParseResult finallyAlone = ParseSource("func F() { try { G(); } finally { H(); } }");
    CHECK(HasError(finallyAlone, "'finally' is not supported"));
    CHECK_FALSE(HasError(finallyAlone, "needs 'catch' arms"));
}

TEST_CASE("throw, throws, try and finally are reserved") {
    for (const std::string_view keyword : {"throw", "throws", "try", "finally"}) {
        CAPTURE(keyword);
        CHECK(KeywordKind(keyword) != TokenKind::Ident);
    }
    CHECK(ParseSource("func F() { let try = 1; }").HasErrors());
}
