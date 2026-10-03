// What a legacy Option `expr?` becomes: a match whose absence arm returns from the enclosing function, carrying the
// destruction of every local that was live at the point the absence left.

#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <algorithm>
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
    variant Option<T> { Some(T), None }
    func Lookup(ok: bool) -> Option<int32> { return Option::Some<int32>(1i32); }
)";
} // namespace

TEST_CASE("propagation lowers to a match that tests the operand's variants") {
    const HirPackage package = LowerSource(kPropagationPrelude + R"(
        func Doubled(ok: bool) -> Option<int32> {
            let value = Lookup(ok)?;
            return Option::Some<int32>(value * 2i32);
        }
    )");

    const HirMatchExpr &match = RequirePropagation(RequireFunction(package, "Doubled"), 0);
    // The operand is evaluated once, as the subject of the match.
    const auto *subject = dynamic_cast<const HirCallExpr *>(match.subject.get());
    REQUIRE(subject != nullptr);
    CHECK_EQ(match.type, TypeRef::MakeInt32());

    REQUIRE_EQ(match.arms.size(), 2);
    const auto *present = dynamic_cast<const HirEnumPattern *>(match.arms[0].pattern.get());
    REQUIRE(present != nullptr);
    CHECK_EQ(present->path, std::vector<std::string>{"Option", "Some"});
    CHECK_EQ(present->form, CaseTypeForm::Variant);
    CHECK(present->hasPayload);
    CHECK_EQ(present->payloadTypes, std::vector<TypeRef>{TypeRef::MakeInt32()});
    const auto *absent = dynamic_cast<const HirEnumPattern *>(match.arms[1].pattern.get());
    REQUIRE(absent != nullptr);
    CHECK_EQ(absent->path, std::vector<std::string>{"Option", "None"});
    CHECK_EQ(absent->form, CaseTypeForm::Variant);
    CHECK_FALSE(absent->hasPayload);

    // The present arm evaluates to the payload the pattern bound, so nothing is copied through a temporary slot.
    const auto *payload = dynamic_cast<const HirVarExpr *>(match.arms[0].body.get());
    REQUIRE(payload != nullptr);
    const auto *bound = dynamic_cast<const HirBindingPattern *>(present->args.front().get());
    REQUIRE(bound != nullptr);
    CHECK_EQ(payload->name, bound->name);
    CHECK_EQ(payload->type, TypeRef::MakeInt32());
}

TEST_CASE("a propagated absence destroys every local that was live when it left") {
    const HirPackage package = LowerSource(kPropagationPrelude + R"(
        struct Handle { value: int32; }
        extend Handle {
            func =(self: &var Handle, other: &Handle);
            func ~Handle(self: &var Handle) {}
        }
        func Guarded(ok: bool) -> Option<int32> {
            let handle = Handle { value: 1i32 };
            let value = Lookup(ok)?;
            return Option::Some<int32>(value);
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
        extend Handle {
            func =(self: &var Handle, other: &Handle);
            func ~Handle(self: &var Handle) {}
        }
        func Later(ok: bool) -> Option<int32> {
            let value = Lookup(ok)?;
            let handle = Handle { value: 1i32 };
            return Option::Some<int32>(value);
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

TEST_CASE("non-generic custom Option variants preserve their concrete payload") {
    const HirPackage package = LowerSource(R"(
        variant Maybe { Some(int32), None }
        func Forward(input: Maybe) -> Maybe {
            let value = input?;
            return Maybe::Some(value);
        }
    )");

    const HirMatchExpr &match = RequirePropagation(RequireFunction(package, "Forward"), 0);
    const auto *present = dynamic_cast<const HirEnumPattern *>(match.arms[0].pattern.get());
    REQUIRE(present != nullptr);
    CHECK_EQ(present->path, std::vector<std::string>{"Maybe", "Some"});
    CHECK_EQ(present->form, CaseTypeForm::Variant);
    REQUIRE_EQ(present->payloadTypes.size(), 1);
    CHECK_EQ(present->payloadTypes.front(), TypeRef::MakeInt32());

    const auto *absent = dynamic_cast<const HirEnumPattern *>(match.arms[1].pattern.get());
    REQUIRE(absent != nullptr);
    CHECK_EQ(absent->path, std::vector<std::string>{"Maybe", "None"});
    CHECK_FALSE(absent->hasPayload);

    const HirReturnStmt &returned = RequireEarlyReturn(match);
    REQUIRE(returned.value.has_value());
    const auto *constructed = dynamic_cast<const HirEnumConstructExpr *>(returned.value->get());
    REQUIRE(constructed != nullptr);
    CHECK_EQ(constructed->form, CaseTypeForm::Variant);
    CHECK_EQ(constructed->type, TypeRef::MakeNamed("Maybe"));
    CHECK(constructed->payloads.empty());
}

TEST_CASE("two propagations in one expression bind their payloads separately") {
    const HirPackage package = LowerSource(kPropagationPrelude + R"(
        func Sum(ok: bool) -> Option<int32> {
            let total = Lookup(ok)? + Lookup(ok)?;
            return Option::Some<int32>(total);
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

TEST_CASE("propagation restores the storage layout of a nested variant payload") {
    const HirPackage package = LowerSource(R"(
        variant Reason { First(uint64), Second(uint64) }
        variant Option<T> { Some(T), None }
        func Forward(input: Option<Reason>) -> Option<int32> {
            let value = input?;
            return Option::Some<int32>(1i32);
        }
    )");
    const HirMatchExpr &match = RequirePropagation(RequireFunction(package, "Forward"), 0);
    const auto *present = dynamic_cast<const HirEnumPattern *>(match.arms[0].pattern.get());
    REQUIRE(present != nullptr);
    REQUIRE_EQ(present->payloadTypes.size(), 1);
    CHECK_EQ(present->payloadTypes.front().SizeInBytes(), 16);
    const auto *bound = dynamic_cast<const HirBindingPattern *>(present->args.front().get());
    REQUIRE(bound != nullptr);
    CHECK_EQ(bound->type.SizeInBytes(), 16);
}

TEST_CASE("propagation captures its absence before deferred statements and ownership cleanup") {
    const HirPackage package = LowerSource(kPropagationPrelude + R"(
        struct Guard { count: *var int32; }
        extend Guard {
            func =(self: &var Guard, other: &Guard);
            func ~Guard(self: &var Guard) { *self.count += 1i32; }
        }
        func Guarded(count: *var int32) -> Option<int32> {
            let guard = Guard { count: count };
            defer *count += 10i32;
            let value = Lookup(false)?;
            return Option::Some<int32>(value);
        }
    )");
    const HirMatchExpr &match = RequirePropagation(RequireFunction(package, "Guarded"), 1);
    const auto *body = dynamic_cast<const HirBlockExpr *>(match.arms[1].body.get());
    REQUIRE(body != nullptr);
    REQUIRE_EQ(body->block.stmts.size(), 1);
    const auto *exit = dynamic_cast<const HirScopeStmt *>(body->block.stmts.front().get());
    REQUIRE(exit != nullptr);
    REQUIRE_EQ(exit->block.stmts.size(), 3);
    const auto *capture = dynamic_cast<const HirLetStmt *>(exit->block.stmts.front().get());
    REQUIRE(capture != nullptr);
    CHECK(dynamic_cast<const HirEnumConstructExpr *>(capture->init.get()) != nullptr);
    CHECK(dynamic_cast<const HirExprStmt *>(exit->block.stmts[1].get()) != nullptr);
    const auto *returned = dynamic_cast<const HirReturnStmt *>(exit->block.stmts.back().get());
    REQUIRE(returned != nullptr);
    const auto *preserved = dynamic_cast<const HirVarExpr *>(returned->value->get());
    REQUIRE(preserved != nullptr);
    CHECK_EQ(preserved->name, capture->name);
    REQUIRE_EQ(returned->cleanups.size(), 1);
    CHECK_EQ(returned->cleanups.front().name, "guard");
}

TEST_CASE("propagation invokes the custom move of the present payload") {
    const HirPackage package = LowerSource(R"(
        variant Option<T> { Some(T), None }
        struct Handle { value: int32; }
        extend Handle {
            func =(self: &var Handle, other: &Handle);
            func <-(self: &var Handle, other: Handle) { self.value = other.value; }
            func ~Handle(self: &var Handle) {}
        }
        func Forward(input: Option<Handle>) -> Option<Handle> {
            let value = (<-input)?;
            return Option::Some<Handle>(<-value);
        }
    )");
    const HirMatchExpr &match = RequirePropagation(RequireFunction(package, "Forward"), 0);
    const auto *present = dynamic_cast<const HirMoveExpr *>(match.arms[0].body.get());
    REQUIRE(present != nullptr);
    CHECK_EQ(present->plan.kind, HirMovePlan::Kind::Custom);
}

TEST_CASE("generic propagation emits only concrete function instantiations") {
    HirPackage hir = LowerSource(R"(
        struct Unit {}
        variant Option<T> { Some(T), None }
        func Forward<T>(input: Option<T>) -> Option<Unit> {
            input?;
            return Option::Some<Unit>(Unit {});
        }
        func Main() -> int32 {
            Forward<uint64>(Option::None<uint64>());
            return 0i32;
        }
    )");
    const HirFunc &declaration = RequireFunction(hir, "Forward");
    CHECK_FALSE(declaration.typeParams.empty());
    HirToLirLowering lowering(std::move(hir), CompileTimeContext{}.target);
    const LirPackage lir = lowering.Generate();
    CHECK(lowering.Diagnostics().empty());
    REQUIRE_EQ(lir.modules.size(), 1);
    const auto &functions = lir.modules.front().funcs;
    CHECK(std::ranges::none_of(functions, [](const LirFunc &function) { return function.name == "Forward"; }));
    CHECK(std::ranges::any_of(functions, [](const LirFunc &function) {
        return function.name.starts_with("Forward") && function.name != "Forward";
    }));
}
