// What a native comparison lowers to: a case-aware comparison with one case per native tag, so equal tags come first
// and only the active payload is compared, an alternative without a payload is equal on its tag alone, and an operand
// with no type of its own is built as the other operand's type.

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
    Lexer lexer(source, "equality.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "equality.rux");
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

const HirAggregateEqualityExpr &ReturnedEquality(const HirPackage &package, const std::string &name) {
    for (const HirModule &module : package.modules) {
        for (const HirFunc &function : module.funcs) {
            if (function.name != name) {
                continue;
            }
            REQUIRE(function.body.has_value());
            const auto *returned = dynamic_cast<const HirReturnStmt *>(function.body->stmts.front().get());
            REQUIRE(returned != nullptr);
            const auto *equality = dynamic_cast<const HirAggregateEqualityExpr *>(returned->value->get());
            REQUIRE(equality != nullptr);
            return *equality;
        }
    }
    FAIL("missing lowered function " << name);
    throw std::runtime_error("missing lowered function");
}
} // namespace

TEST_CASE("a native comparison has one case per native tag") {
    const HirPackage package = LowerSource(R"(
        struct E { code: int32; }
        func Channels(left: int32 ! E, right: int32 ! E) -> bool {
            return left == right;
        }
        func Presence(left: int32?, right: int32?) -> bool {
            return left != right;
        }
    )");
    const HirAggregateEqualityExpr &channels = ReturnedEquality(package, "Channels");
    CHECK_FALSE(channels.negated);
    REQUIRE_EQ(channels.plan.operation, HirVariantEqualityPayload::Operation::Variant);
    REQUIRE_EQ(channels.plan.variantCases.size(), 2);
    CHECK_EQ(channels.plan.variantCases[0].discriminant, "0");
    REQUIRE_EQ(channels.plan.variantCases[0].payloads.size(), 1);
    CHECK_EQ(channels.plan.variantCases[0].payloads.front().type, TypeRef::MakeInt32());
    CHECK_EQ(channels.plan.variantCases[1].payloads.front().type.ToString(), "E");

    const HirAggregateEqualityExpr &presence = ReturnedEquality(package, "Presence");
    CHECK(presence.negated);
    REQUIRE_EQ(presence.plan.variantCases.size(), 2);
    // Absence carries no payload, so equal absent tags are equal on their own.
    CHECK(presence.plan.variantCases[0].payloads.empty());
    CHECK_EQ(presence.plan.variantCases[1].payloads.size(), 1);
}

TEST_CASE("an untyped operand is built as the other operand's type") {
    const HirPackage package = LowerSource(R"(
        struct E {}
        func Constructor(value: int32 ! E) -> bool {
            return value == .Success(1i32);
        }
        func Literal(value: int64 | bool) -> bool {
            return 5 == value;
        }
        func Wrapped(index: uint?) -> bool {
            return index == .Some(6);
        }
    )");
    const HirAggregateEqualityExpr &constructor = ReturnedEquality(package, "Constructor");
    const auto *built = dynamic_cast<const HirEnumConstructExpr *>(constructor.right.get());
    REQUIRE(built != nullptr);
    CHECK_EQ(built->type, constructor.left->type);
    CHECK_EQ(built->discriminant, "0");

    const HirAggregateEqualityExpr &literal = ReturnedEquality(package, "Literal");
    const auto *member = dynamic_cast<const HirEnumConstructExpr *>(literal.left.get());
    REQUIRE(member != nullptr);
    // Canonical order spells `bool8 | int64`, so the integer is member one, at the member's own width.
    CHECK_EQ(member->discriminant, "1");
    REQUIRE_EQ(member->payloads.size(), 1);
    CHECK_EQ(member->payloads.front()->type, TypeRef::MakeInt64());

    // A constructor around an unsuffixed literal has no type of its own either, so `.Some(6)` is a `uint?`.
    const HirAggregateEqualityExpr &wrapped = ReturnedEquality(package, "Wrapped");
    const auto *present = dynamic_cast<const HirEnumConstructExpr *>(wrapped.right.get());
    REQUIRE(present != nullptr);
    CHECK_EQ(present->type, wrapped.left->type);
    REQUIRE_EQ(present->payloads.size(), 1);
    CHECK_EQ(present->payloads.front()->type, wrapped.left->type.inner.front());
}
