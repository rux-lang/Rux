// What `expr?` becomes: a match whose failure arm returns from the enclosing function, carrying the destruction of
// every local that was live at the point the failure left.

#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <doctest.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace Rux;

namespace {
HirPackage LowerSource(const std::string &source) {
    Lexer lexer(source, "propagation.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "propagation.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());
    return AstToHirLowering(model).Generate();
}

const HirFunc &RequireFunction(const HirPackage &package, const std::string &name) {
    REQUIRE_EQ(package.modules.size(), 1);
    for (const HirFunc &function : package.modules.front().funcs) {
        if (function.name == name) {
            return function;
        }
    }
    FAIL("missing lowered function " << name);
    throw std::runtime_error("missing lowered function");
}

/// The match a propagation lowers to, taken from the initializer of the nth `let` in a function body.
const HirMatchExpr &RequirePropagation(const HirFunc &function, const std::size_t statement) {
    REQUIRE(function.body.has_value());
    REQUIRE_GT(function.body->stmts.size(), statement);
    const auto *binding = dynamic_cast<const HirLetStmt *>(function.body->stmts[statement].get());
    REQUIRE(binding != nullptr);
    REQUIRE(binding->init != nullptr);
    const auto *match = dynamic_cast<const HirMatchExpr *>(binding->init.get());
    REQUIRE(match != nullptr);
    return *match;
}

/// The return statement a propagation's failure arm performs.
const HirReturnStmt &RequireEarlyReturn(const HirMatchExpr &match) {
    REQUIRE_EQ(match.arms.size(), 2);
    const auto *body = dynamic_cast<const HirBlockExpr *>(match.arms[1].body.get());
    REQUIRE(body != nullptr);
    REQUIRE_EQ(body->block.stmts.size(), 1);
    const auto *returned = dynamic_cast<const HirReturnStmt *>(body->block.stmts.front().get());
    REQUIRE(returned != nullptr);
    return *returned;
}

const std::string kPropagationPrelude = R"(
    interface Drop {}
    enum ParseError: int32 { Bad }
    enum Result<T, E> { Success(T), Error(E) }
    enum Option<T> { Some(T), None }
    func Read(ok: bool) -> Result<int32, ParseError> {
        return Result::Success<int32, ParseError>(7i32);
    }
    func Lookup(ok: bool) -> Option<int32> { return Option::Some<int32>(1i32); }
)";
} // namespace

TEST_CASE("propagation lowers to a match that tests the operand's variants") {
    const HirPackage package = LowerSource(kPropagationPrelude + R"(
        func Doubled(ok: bool) -> Result<int32, ParseError> {
            let value = Read(ok)?;
            return Result::Success<int32, ParseError>(value * 2i32);
        }
    )");

    const HirMatchExpr &match = RequirePropagation(RequireFunction(package, "Doubled"), 0);
    // The operand is evaluated once, as the subject of the match.
    const auto *subject = dynamic_cast<const HirCallExpr *>(match.subject.get());
    REQUIRE(subject != nullptr);
    CHECK_EQ(match.type, TypeRef::MakeInt32());

    REQUIRE_EQ(match.arms.size(), 2);
    const auto *success = dynamic_cast<const HirEnumPattern *>(match.arms[0].pattern.get());
    REQUIRE(success != nullptr);
    CHECK_EQ(success->path, std::vector<std::string>{"Result", "Success"});
    CHECK(success->hasPayload);
    const auto *failure = dynamic_cast<const HirEnumPattern *>(match.arms[1].pattern.get());
    REQUIRE(failure != nullptr);
    CHECK_EQ(failure->path, std::vector<std::string>{"Result", "Error"});
    CHECK(failure->hasPayload);

    // The success arm evaluates to the payload the pattern bound, so nothing is copied through a temporary slot.
    const auto *payload = dynamic_cast<const HirVarExpr *>(match.arms[0].body.get());
    REQUIRE(payload != nullptr);
    const auto *bound = dynamic_cast<const HirBindingPattern *>(success->args.front().get());
    REQUIRE(bound != nullptr);
    CHECK_EQ(payload->name, bound->name);
    CHECK_EQ(payload->type, TypeRef::MakeInt32());
}

TEST_CASE("the failure arm returns the enclosing function's failure variant carrying the same payload") {
    const HirPackage package = LowerSource(kPropagationPrelude + R"(
        func Doubled(ok: bool) -> Result<int32, ParseError> {
            let value = Read(ok)?;
            return Result::Success<int32, ParseError>(value);
        }
    )");

    const HirMatchExpr &match = RequirePropagation(RequireFunction(package, "Doubled"), 0);
    const HirReturnStmt &returned = RequireEarlyReturn(match);
    REQUIRE(returned.value.has_value());
    const auto *constructed = dynamic_cast<const HirEnumConstructExpr *>(returned.value->get());
    REQUIRE(constructed != nullptr);
    CHECK_EQ(constructed->type, TypeRef::MakeNamed("Result<int32, ParseError>"));
    REQUIRE_EQ(constructed->payloads.size(), 1);

    const auto *carried = dynamic_cast<const HirVarExpr *>(constructed->payloads.front().get());
    REQUIRE(carried != nullptr);
    const auto *failurePattern = dynamic_cast<const HirEnumPattern *>(match.arms[1].pattern.get());
    REQUIRE(failurePattern != nullptr);
    const auto *bound = dynamic_cast<const HirBindingPattern *>(failurePattern->args.front().get());
    REQUIRE(bound != nullptr);
    CHECK_EQ(carried->name, bound->name);
    CHECK_EQ(carried->type, TypeRef::MakeNamed("ParseError"));
}

TEST_CASE("a propagated failure destroys every local that was live when it left") {
    const HirPackage package = LowerSource(kPropagationPrelude + R"(
        struct Handle { value: int32; }
        extend Handle : Drop {}
        func Guarded(ok: bool) -> Result<int32, ParseError> {
            let handle = Handle { value: 1i32 };
            let value = Read(ok)?;
            return Result::Success<int32, ParseError>(value);
        }
    )");

    const HirFunc &guarded = RequireFunction(package, "Guarded");
    const HirReturnStmt &early = RequireEarlyReturn(RequirePropagation(guarded, 1));
    REQUIRE_EQ(early.cleanups.size(), 1);
    CHECK_EQ(early.cleanups.front().name, "handle");
    CHECK_EQ(early.cleanups.front().type, TypeRef::MakeNamed("Handle"));
    CHECK_FALSE(early.cleanups.front().glueSymbol.empty());
}

TEST_CASE("a local declared after the propagation is not destroyed by it") {
    const HirPackage package = LowerSource(kPropagationPrelude + R"(
        struct Handle { value: int32; }
        extend Handle : Drop {}
        func Later(ok: bool) -> Result<int32, ParseError> {
            let value = Read(ok)?;
            let handle = Handle { value: 1i32 };
            return Result::Success<int32, ParseError>(value);
        }
    )");

    const HirReturnStmt &early = RequireEarlyReturn(RequirePropagation(RequireFunction(package, "Later"), 0));
    CHECK(early.cleanups.empty());
}

TEST_CASE("propagating an Option returns its payload-less failure variant") {
    const HirPackage package = LowerSource(kPropagationPrelude + R"(
        func Found(ok: bool) -> Option<int32> {
            let value = Lookup(ok)?;
            return Option::Some<int32>(value);
        }
    )");

    const HirMatchExpr &match = RequirePropagation(RequireFunction(package, "Found"), 0);
    const auto *failurePattern = dynamic_cast<const HirEnumPattern *>(match.arms[1].pattern.get());
    REQUIRE(failurePattern != nullptr);
    CHECK_EQ(failurePattern->path, std::vector<std::string>{"Option", "None"});
    CHECK_FALSE(failurePattern->hasPayload);

    const HirReturnStmt &returned = RequireEarlyReturn(match);
    REQUIRE(returned.value.has_value());
    const auto *constructed = dynamic_cast<const HirEnumConstructExpr *>(returned.value->get());
    REQUIRE(constructed != nullptr);
    CHECK(constructed->payloads.empty());
}

TEST_CASE("two propagations in one expression bind their payloads separately") {
    const HirPackage package = LowerSource(kPropagationPrelude + R"(
        func Sum(ok: bool) -> Result<int32, ParseError> {
            let total = Read(ok)? + Read(ok)?;
            return Result::Success<int32, ParseError>(total);
        }
    )");

    const HirFunc &sum = RequireFunction(package, "Sum");
    REQUIRE(sum.body.has_value());
    const auto *binding = dynamic_cast<const HirLetStmt *>(sum.body->stmts.front().get());
    REQUIRE(binding != nullptr);
    const auto *added = dynamic_cast<const HirBinaryExpr *>(binding->init.get());
    REQUIRE(added != nullptr);

    const auto boundName = [](const HirExpr &expr) {
        const auto *match = dynamic_cast<const HirMatchExpr *>(&expr);
        REQUIRE(match != nullptr);
        const auto *pattern = dynamic_cast<const HirEnumPattern *>(match->arms.front().pattern.get());
        REQUIRE(pattern != nullptr);
        const auto *bound = dynamic_cast<const HirBindingPattern *>(pattern->args.front().get());
        REQUIRE(bound != nullptr);
        return bound->name;
    };
    CHECK_NE(boundName(*added->left), boundName(*added->right));
}
