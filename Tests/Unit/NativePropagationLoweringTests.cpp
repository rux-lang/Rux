// What native propagation lowers to: a match over the evaluated operand whose success arm moves the payload onward and
// whose failure arm is an ordinary return of the enclosing outer failure, the error injected or widened into its
// channel; and a fallible `Main` behind a synthesized integer entry point.

#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <doctest.h>
#include <stdexcept>
#include <string>
#include <utility>

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
    AstToHirLowering lowering(model);
    HirPackage package = lowering.Generate();
    REQUIRE(lowering.Diagnostics().empty());
    return package;
}

const HirFunc &RequireFunction(const HirPackage &package, const std::string &name) {
    for (const HirModule &module : package.modules) {
        for (const HirFunc &function : module.funcs) {
            if (function.name == name) {
                return function;
            }
        }
    }
    FAIL("missing lowered function " << name);
    throw std::runtime_error("missing lowered function");
}

const HirMatchExpr &LetMatch(const HirFunc &function) {
    REQUIRE(function.body.has_value());
    const auto *let = dynamic_cast<const HirLetStmt *>(function.body->stmts.front().get());
    REQUIRE(let != nullptr);
    const auto *match = dynamic_cast<const HirMatchExpr *>(let->init.get());
    REQUIRE(match != nullptr);
    return *match;
}

const HirEnumConstructExpr &ReturnedFailure(const HirMatchArm &arm) {
    const auto *block = dynamic_cast<const HirBlockExpr *>(arm.body.get());
    REQUIRE(block != nullptr);
    REQUIRE_EQ(block->block.stmts.size(), 1);
    const auto *returned = dynamic_cast<const HirReturnStmt *>(block->block.stmts.front().get());
    REQUIRE(returned != nullptr);
    REQUIRE(returned->value.has_value());
    const auto *failure = dynamic_cast<const HirEnumConstructExpr *>(returned->value->get());
    REQUIRE(failure != nullptr);
    return *failure;
}
} // namespace

TEST_CASE("propagation continues with the success or returns the outer failure") {
    const HirPackage package = LowerSource(R"(
        struct ParseError {}
        struct IoError {}
        func Read() -> (int32 ! ParseError) ! IoError { return .Success(.Success(1i32)); }
        func Forward() -> int32 ! IoError {
            let inner = Read()?;
            return 1i32;
        }
    )");
    const HirMatchExpr &match = LetMatch(RequireFunction(package, "Forward"));
    REQUIRE_EQ(match.arms.size(), 2);
    // The inner fallible continues untouched as the success payload.
    CHECK_EQ(match.type.ToString(), "int32 ! ParseError");
    const auto *success = dynamic_cast<const HirEnumPattern *>(match.arms[0].pattern.get());
    REQUIRE(success != nullptr);
    CHECK_EQ(success->discriminant, "0");
    const HirEnumConstructExpr &failure = ReturnedFailure(match.arms[1]);
    CHECK_EQ(failure.discriminant, "1");
    CHECK_EQ(failure.type.ToString(), "int32 ! IoError");
    REQUIRE_EQ(failure.payloads.size(), 1);
    CHECK_EQ(failure.payloads.front()->type.ToString(), "IoError");
}

TEST_CASE("a propagated error is injected into a wider failure channel") {
    const HirPackage package = LowerSource(R"(
        struct ParseError {}
        struct IoError {}
        func Read() -> int32 ! ParseError { return 1i32; }
        func Widen() -> int32 ! (IoError | ParseError) {
            let value = Read()?;
            return value;
        }
    )");
    const HirEnumConstructExpr &failure = ReturnedFailure(LetMatch(RequireFunction(package, "Widen")).arms[1]);
    REQUIRE_EQ(failure.payloads.size(), 1);
    // `ParseError` is member one of `IoError | ParseError`.
    const auto *injected = dynamic_cast<const HirEnumConstructExpr *>(failure.payloads.front().get());
    REQUIRE(injected != nullptr);
    CHECK_EQ(injected->discriminant, "1");
}

TEST_CASE("a fallible Main is wrapped by an integer entry point") {
    const HirPackage package = LowerSource(R"(
        struct IoError {}
        func Main() -> int ! IoError {
            return 3;
        }
    )");
    const HirFunc &entry = RequireFunction(package, "Main");
    CHECK_EQ(entry.returnType, TypeRef::MakeInt());
    const HirFunc &body = RequireFunction(package, "Main$fallible");
    CHECK(body.returnType.IsFallible());

    REQUIRE(entry.body.has_value());
    const auto *returned = dynamic_cast<const HirReturnStmt *>(entry.body->stmts.front().get());
    REQUIRE(returned != nullptr);
    const auto *match = dynamic_cast<const HirMatchExpr *>(returned->value->get());
    REQUIRE(match != nullptr);
    CHECK(match->subject->consumption.has_value());
    REQUIRE_EQ(match->arms.size(), 2);
    const auto *failed = dynamic_cast<const HirLiteralExpr *>(match->arms[1].body.get());
    REQUIRE(failed != nullptr);
    CHECK_EQ(failed->value, "1");
}
