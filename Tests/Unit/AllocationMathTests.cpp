// Compile-time layout queries and the checked arithmetic an allocation size is computed with.

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
std::vector<SemanticDiagnostic> AnalyzeSource(const std::string &source) {
    Lexer lexer(source, "allocation.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "allocation.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    return analyzer.Analyze().diagnostics;
}

HirPackage LowerSource(const std::string &source) {
    Lexer lexer(source, "allocation.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "allocation.rux");
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

/// The initializer of the nth `let` in a function body.
const HirExpr &RequireInitializer(const HirFunc &function, const std::size_t statement) {
    REQUIRE(function.body.has_value());
    REQUIRE_GT(function.body->stmts.size(), statement);
    const auto *binding = dynamic_cast<const HirLetStmt *>(function.body->stmts[statement].get());
    REQUIRE(binding != nullptr);
    REQUIRE(binding->init != nullptr);
    return *binding->init;
}

const std::string kCheckedPrelude = R"(
    intrinsic func CheckedAdd(left: uint64, right: uint64, result: *var uint64) -> bool;
    intrinsic func CheckedMul(left: uint64, right: uint64, result: *var uint64) -> bool;
)";
} // namespace

TEST_CASE("both layout queries fold to unsigned constants") {
    const HirPackage package = LowerSource(R"(
        struct Header { tag: uint32; payload: uint64; }
        func Query() -> uint64 {
            let size = sizeof(Header);
            let alignment = alignof(Header);
            let elementSize = sizeof(uint32);
            return size + alignment + elementSize;
        }
    )");

    const HirFunc &query = RequireFunction(package, "Query");
    const auto folded = [&](const std::size_t statement) {
        const auto *literal = dynamic_cast<const HirLiteralExpr *>(&RequireInitializer(query, statement));
        REQUIRE(literal != nullptr);
        CHECK_EQ(literal->type, TypeRef::MakeUInt64());
        return literal->value;
    };
    CHECK_EQ(folded(0), "16");
    CHECK_EQ(folded(1), "8");
    CHECK_EQ(folded(2), "4");
}

TEST_CASE("a layout query over a type with no layout is reported once") {
    const auto diagnostics = AnalyzeSource(R"(
        struct Node { next: Node; value: int32; }
        func Query() -> uint64 { return alignof(Node); }
    )");

    REQUIRE_FALSE(diagnostics.empty());
    const auto reported = std::ranges::find_if(diagnostics, [](const SemanticDiagnostic &diagnostic) {
        return diagnostic.message.starts_with("cannot determine the alignment");
    });
    REQUIRE(reported != diagnostics.end());
    CHECK_EQ(reported->message, "cannot determine the alignment of type 'Node'");
    REQUIRE_EQ(reported->notes.size(), 1);
    CHECK_EQ(reported->notes[0], "'alignof' needs a type whose layout is known at compile time");
}

TEST_CASE("a checked operation becomes the arithmetic and its check, not a call") {
    const HirPackage package = LowerSource(kCheckedPrelude + R"(
        func Size(count: uint64) -> bool {
            var bytes = 0u64;
            let overflowed = CheckedMul(count, 8u64, @bytes);
            return overflowed;
        }
    )");

    const HirFunc &size = RequireFunction(package, "Size");
    const auto *inlined = dynamic_cast<const HirBlockExpr *>(&RequireInitializer(size, 1));
    REQUIRE(inlined != nullptr);
    CHECK_EQ(inlined->type, TypeRef::MakeBool());
    // Both operands and the result are named once, then the product is stored through the caller's pointer.
    REQUIRE_EQ(inlined->block.stmts.size(), 5);
    CHECK(dynamic_cast<const HirLetStmt *>(inlined->block.stmts[0].get()) != nullptr);
    CHECK(dynamic_cast<const HirLetStmt *>(inlined->block.stmts[3].get()) != nullptr);
    CHECK(dynamic_cast<const HirExprStmt *>(inlined->block.stmts[4].get()) != nullptr);

    // The report is the comparison, so the caller branches on a value rather than on a trap.
    REQUIRE(inlined->value != nullptr);
    const auto *reported = dynamic_cast<const HirBinaryExpr *>(inlined->value.get());
    REQUIRE(reported != nullptr);
    CHECK_EQ(reported->op, TokenKind::AmpAmp);
}

TEST_CASE("a checked sum reports the wrap by comparing against an operand") {
    const HirPackage package = LowerSource(kCheckedPrelude + R"(
        func Sum(left: uint64, right: uint64) -> bool {
            var total = 0u64;
            let overflowed = CheckedAdd(left, right, @total);
            return overflowed;
        }
    )");

    const auto *inlined = dynamic_cast<const HirBlockExpr *>(&RequireInitializer(RequireFunction(package, "Sum"), 1));
    REQUIRE(inlined != nullptr);
    const auto *reported = dynamic_cast<const HirBinaryExpr *>(inlined->value.get());
    REQUIRE(reported != nullptr);
    CHECK_EQ(reported->op, TokenKind::Less);
}

TEST_CASE("a checked intrinsic declared with the wrong signature is rejected") {
    const auto wrongOperand = AnalyzeSource(R"(
        intrinsic func CheckedAdd(left: uint32, right: uint64, result: *var uint64) -> bool;
    )");

    REQUIRE_EQ(wrongOperand.size(), 1);
    CHECK_EQ(wrongOperand[0].message, "checked arithmetic intrinsic 'CheckedAdd' must be declared as "
                                      "'func CheckedAdd(left: uint64, right: uint64, result: *var uint64) -> bool'");
    REQUIRE_EQ(wrongOperand[0].notes.size(), 1);
    CHECK_EQ(wrongOperand[0].notes[0], "parameter 'left' has type 'uint32'");

    const auto readOnlyResult = AnalyzeSource(R"(
        intrinsic func CheckedMul(left: uint64, right: uint64, result: *uint64) -> bool;
    )");

    REQUIRE_EQ(readOnlyResult.size(), 1);
    CHECK_EQ(readOnlyResult[0].notes[0], "parameter 'result' has type '*uint64'");

    const auto wrongArity = AnalyzeSource(R"(
        intrinsic func CheckedSub(left: uint64, right: uint64) -> bool;
    )");

    REQUIRE_EQ(wrongArity.size(), 1);
    CHECK_EQ(wrongArity[0].notes[0], "it declares 2 parameters");

    const auto wrongReturn = AnalyzeSource(R"(
        intrinsic func CheckedAdd(left: uint64, right: uint64, result: *var uint64) -> uint64;
    )");

    REQUIRE_EQ(wrongReturn.size(), 1);
    CHECK_EQ(wrongReturn[0].notes[0], "it returns 'uint64'");
}

TEST_CASE("a correctly declared checked intrinsic is accepted") {
    const auto diagnostics = AnalyzeSource(kCheckedPrelude);

    CHECK(diagnostics.empty());
}
