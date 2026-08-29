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

using namespace Rux;

namespace {
HirPackage DropHir(const std::string &source) {
    Lexer lexer(source, "variant-drop.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "variant-drop.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", CompileTimeContext{});
    const SemanticModel model = analyzer.Analyze();
    for (const SemanticDiagnostic &diagnostic : model.diagnostics) {
        INFO(diagnostic.message);
    }
    REQUIRE_FALSE(model.HasErrors());
    AstToHirLowering lowering(model);
    HirPackage package = lowering.Generate();
    REQUIRE(lowering.Diagnostics().empty());
    return package;
}

LirPackage DropLir(const std::string &source, const TargetContext target = TargetContext::CreateNative()) {
    HirToLirLowering lowering(DropHir(source), target);
    LirPackage package = lowering.Generate();
    for (const Diagnostic &diagnostic : lowering.Diagnostics()) {
        INFO(diagnostic.message);
    }
    REQUIRE(lowering.Diagnostics().empty());
    return package;
}

const DropGluePlan &DropPlan(const HirPackage &package, const std::string &type) {
    const auto found = std::ranges::find(package.dropGlues, TypeRef::MakeNamed(type), &DropGluePlan::type);
    REQUIRE_MESSAGE(found != package.dropGlues.end(), type);
    return *found;
}

const HirFunc &HirFunction(const HirPackage &package, const std::string &name) {
    for (const HirModule &module : package.modules) {
        for (const HirFunc &function : module.funcs) {
            if (function.name == name) {
                return function;
            }
        }
    }
    FAIL("missing HIR function " << name);
    throw std::runtime_error("missing HIR function");
}

const LirFunc &LirFunction(const LirPackage &package, const std::string &name) {
    for (const LirModule &module : package.modules) {
        for (const LirFunc &function : module.funcs) {
            if (function.name == name) {
                return function;
            }
        }
    }
    FAIL("missing LIR function " << name);
    throw std::runtime_error("missing LIR function");
}

std::size_t Calls(const LirFunc &function, const std::string &symbol) {
    std::size_t count = 0;
    for (const LirBlock &block : function.blocks) {
        count += std::ranges::count_if(block.instrs, [&](const LirInstr &instruction) {
            return instruction.op == LirOpcode::Call && instruction.strArg == symbol;
        });
    }
    return count;
}

std::size_t Blocks(const LirFunc &function, const std::string_view prefix) {
    return std::ranges::count_if(function.blocks,
                                 [&](const LirBlock &block) { return block.label.starts_with(prefix); });
}

const std::string kOwner = R"(
    struct Handle { value: int32; }
    extend Handle {
        func =(self: &var Handle, other: &Handle);
        func ~Handle(self: &var Handle) { self.value = 0i32; }
    }
)";
} // namespace

TEST_CASE("variant drop plans retain declared form and only droppable active cases") {
    const HirPackage package = DropHir(kOwner + R"(
        variant Choice {
            Empty,
            Number(int32),
            Direct(Handle),
            Pair(Handle, int32, Handle)
        }
        func Use(value: Choice) {}
    )");
    const DropGluePlan &plan = DropPlan(package, "Choice");
    REQUIRE_EQ(plan.steps.size(), 2);
    CHECK_EQ(plan.steps[0].form, CaseTypeForm::Variant);
    CHECK_EQ(plan.steps[0].name, "Direct");
    CHECK_EQ(plan.steps[0].ordinal, 2);
    CHECK_EQ(plan.steps[0].discriminant, "2");
    REQUIRE_EQ(plan.steps[0].children.size(), 1);
    CHECK_EQ(plan.steps[0].children[0].ordinal, 0);
    CHECK_EQ(plan.steps[1].form, CaseTypeForm::Variant);
    CHECK_EQ(plan.steps[1].name, "Pair");
    CHECK_EQ(plan.steps[1].discriminant, "3");
    REQUIRE_EQ(plan.steps[1].payloadTypes.size(), 3);
    REQUIRE_EQ(plan.steps[1].children.size(), 2);
    CHECK_EQ(plan.steps[1].children[0].ordinal, 2);
    CHECK_EQ(plan.steps[1].children[1].ordinal, 0);
}

TEST_CASE("named variant payload drop order is reverse declaration order") {
    const HirPackage package = DropHir(kOwner + R"(
        variant Record {
            Empty,
            Value { first: Handle; count: int32; second: Handle; }
        }
        func Use(value: Record) {}
    )");
    const DropGluePlan &plan = DropPlan(package, "Record");
    REQUIRE_EQ(plan.steps.size(), 1);
    const DropGlueStep &value = plan.steps.front();
    CHECK_EQ(value.form, CaseTypeForm::Variant);
    CHECK_EQ(value.name, "Value");
    REQUIRE_EQ(value.payloadTypes.size(), 3);
    CHECK_EQ(value.payloadTypes[0], TypeRef::MakeNamed("Handle"));
    CHECK_EQ(value.payloadTypes[1], TypeRef::MakeInt32());
    CHECK_EQ(value.payloadTypes[2], TypeRef::MakeNamed("Handle"));
    REQUIRE_EQ(value.children.size(), 2);
    CHECK_EQ(value.children[0].name, "second");
    CHECK_EQ(value.children[0].ordinal, 2);
    CHECK_EQ(value.children[1].name, "first");
    CHECK_EQ(value.children[1].ordinal, 0);
}

TEST_CASE("nested generic variants retain concrete recursive destruction") {
    const HirPackage package = DropHir(kOwner + R"(
        variant Maybe<T> { None, Some(T) }
        variant Envelope<T> { Empty, Direct(T), Nested(Maybe<T>) }
        func Use(value: Envelope<Handle>, inner: Maybe<Handle>) {}
    )");
    const DropGluePlan &inner = DropPlan(package, "Maybe<Handle>");
    REQUIRE_EQ(inner.steps.size(), 1);
    CHECK_EQ(inner.steps.front().name, "Some");
    CHECK_EQ(inner.steps.front().payloadTypes, std::vector<TypeRef>{TypeRef::MakeNamed("Handle")});
    const DropGluePlan &outer = DropPlan(package, "Envelope<Handle>");
    REQUIRE_EQ(outer.steps.size(), 2);
    CHECK_EQ(outer.steps[0].name, "Direct");
    CHECK_EQ(outer.steps[1].name, "Nested");
    REQUIRE_EQ(outer.steps[1].children.size(), 1);
    REQUIRE_EQ(outer.steps[1].children[0].children.size(), 1);
    CHECK_EQ(outer.steps[1].children[0].children[0].form, CaseTypeForm::Variant);
    CHECK_EQ(outer.steps[1].children[0].children[0].name, "Some");
}

TEST_CASE("scalar enums and all-trivial variants synthesize no destruction") {
    const HirPackage package = DropHir(R"(
        enum Status: uint8 { Ready = 1, Busy = 2 }
        variant State { Idle, Ready, Complete }
        variant Number { Empty, Value(int64) }
        func Use(status: Status, state: State, number: Number) {}
    )");
    CHECK(std::ranges::none_of(package.dropGlues, [](const DropGluePlan &plan) {
        return plan.type.name == "Status" || plan.type.name == "State" || plan.type.name == "Number";
    }));
}

TEST_CASE("drop lowering branches on each droppable case before reading payloads") {
    const LirPackage package = DropLir(kOwner + R"(
        variant Choice { Empty, First(Handle), Plain(int64), Pair(Handle, Handle) }
        func Use(value: Choice) {}
    )");
    const std::string symbol =
        std::ranges::find(package.dropGlues, TypeRef::MakeNamed("Choice"), &DropGluePlan::type)->symbol;
    const LirFunc &glue = LirFunction(package, symbol);
    CHECK_EQ(Blocks(glue, "drop.variant"), 4);
    CHECK_EQ(Blocks(glue, "drop.variant.after"), 2);
    CHECK_EQ(Calls(glue, "Handle::~Handle"), 3);
    for (const LirBlock &block : glue.blocks) {
        if (block.label.starts_with("drop.variant") && !block.label.starts_with("drop.variant.after")) {
            CHECK(std::ranges::any_of(
                block.instrs, [](const LirInstr &instruction) { return instruction.op == LirOpcode::IndexPtr; }));
        }
    }
}

TEST_CASE("tuple variant construction records reverse partial cleanup with variant form") {
    const HirPackage package = DropHir(kOwner + R"(
        variant Choice { Empty, Pair(Handle, Handle, int32) }
        func Build(first: Handle, second: Handle) -> Choice {
            return Choice::Pair(<-first, <-second, 7i32);
        }
    )");
    const HirFunc &build = HirFunction(package, "Build");
    REQUIRE(build.body.has_value());
    const auto *returned = dynamic_cast<const HirReturnStmt *>(build.body->stmts.front().get());
    REQUIRE(returned != nullptr);
    const auto *construction = dynamic_cast<const HirEnumConstructExpr *>(returned->value->get());
    REQUIRE(construction != nullptr);
    CHECK_EQ(construction->form, CaseTypeForm::Variant);
    REQUIRE_EQ(construction->failureCleanups.size(), 3);
    CHECK(construction->failureCleanups[0].empty());
    REQUIRE_EQ(construction->failureCleanups[1].size(), 1);
    CHECK_EQ(construction->failureCleanups[1][0].form, CaseTypeForm::Variant);
    CHECK_EQ(construction->failureCleanups[1][0].kind, HirPartialDropAction::Kind::EnumPayload);
    CHECK_EQ(construction->failureCleanups[1][0].ordinal, 0);
    REQUIRE_EQ(construction->failureCleanups[2].size(), 2);
    CHECK_EQ(construction->failureCleanups[2][0].ordinal, 1);
    CHECK_EQ(construction->failureCleanups[2][1].ordinal, 0);
}

TEST_CASE("named variant construction records completed payload cleanup by field") {
    const HirPackage package = DropHir(kOwner + R"(
        variant Record { Empty, Value { first: Handle; count: int32; second: Handle; } }
        func Build(first: Handle, second: Handle) -> Record {
            return Record::Value { first: <-first, count: 7i32, second: <-second };
        }
    )");
    const HirFunc &build = HirFunction(package, "Build");
    REQUIRE(build.body.has_value());
    const auto *returned = dynamic_cast<const HirReturnStmt *>(build.body->stmts.front().get());
    REQUIRE(returned != nullptr);
    const auto *construction = dynamic_cast<const HirEnumConstructExpr *>(returned->value->get());
    REQUIRE(construction != nullptr);
    REQUIRE_EQ(construction->failureCleanups.size(), 3);
    REQUIRE_EQ(construction->failureCleanups[2].size(), 1);
    const HirPartialDropAction &first = construction->failureCleanups[2].front();
    CHECK_EQ(first.form, CaseTypeForm::Variant);
    CHECK_EQ(first.kind, HirPartialDropAction::Kind::EnumPayload);
    CHECK_EQ(first.name, "first");
    CHECK_EQ(first.ordinal, 0);
}

TEST_CASE("variant values in returns and loops keep one flag-guarded glue call per binding") {
    const LirPackage package = DropLir(kOwner + R"(
        variant Held { Empty, Full(Handle) }
        func Make(value: Handle) -> Held { return Held::Full(<-value); }
        func Run(flag: bool) {
            loop {
                let held = Make(Handle { value: 1i32 });
                if flag { break; }
                break;
            }
        }
    )");
    const auto plan = std::ranges::find(package.dropGlues, TypeRef::MakeNamed("Held"), &DropGluePlan::type);
    REQUIRE(plan != package.dropGlues.end());
    const LirFunc &make = LirFunction(package, "Make");
    const LirFunc &run = LirFunction(package, "Run");
    CHECK_EQ(Calls(make, plan->symbol), 0);
    CHECK_GE(Calls(run, plan->symbol), 1);
    CHECK_EQ(Calls(make, "Handle::~Handle"), 0);
    CHECK(std::ranges::any_of(
        run.blocks, [](const LirBlock &block) { return block.term && block.term->kind == LirTermKind::Branch; }));
}

TEST_CASE("variant direct destructors run before active payload destruction") {
    const HirPackage hir = DropHir(kOwner + R"(
        variant Held { Empty, Full(Handle) }
        extend Held {
            func ~Held(self: &var Held) {}
        }
        func Use(value: Held) {}
    )");
    const DropGluePlan &plan = DropPlan(hir, "Held");
    REQUIRE_EQ(plan.steps.size(), 2);
    CHECK_EQ(plan.steps[0].kind, DropGlueStep::Kind::InvokeDrop);
    CHECK_EQ(plan.steps[0].dropSymbol, "Held::~Held");
    CHECK_EQ(plan.steps[1].kind, DropGlueStep::Kind::EnumVariant);
    CHECK_EQ(plan.steps[1].form, CaseTypeForm::Variant);

    const LirPackage lir = DropLir(kOwner + R"(
        variant Held { Empty, Full(Handle) }
        extend Held {
            func ~Held(self: &var Held) {}
        }
        func Use(value: Held) {}
    )");
    const auto lirPlan = std::ranges::find(lir.dropGlues, TypeRef::MakeNamed("Held"), &DropGluePlan::type);
    REQUIRE(lirPlan != lir.dropGlues.end());
    const LirFunc &glue = LirFunction(lir, lirPlan->symbol);
    CHECK_EQ(Calls(glue, "Held::~Held"), 1);
    CHECK_EQ(Calls(glue, "Handle::~Handle"), 1);
    CHECK_EQ(Blocks(glue, "drop.variant"), 2);
}

TEST_CASE("scalar enum destruction never enters variant payload dispatch") {
    const LirPackage package = DropLir(R"(
        enum Status: uint8 { Ready = 1, Busy = 2 }
        extend Status {
            func ~Status(self: &var Status) {}
        }
        func Use(value: Status) {}
    )");
    const auto plan = std::ranges::find(package.dropGlues, TypeRef::MakeNamed("Status"), &DropGluePlan::type);
    REQUIRE(plan != package.dropGlues.end());
    REQUIRE_EQ(plan->steps.size(), 1);
    CHECK_EQ(plan->steps.front().kind, DropGlueStep::Kind::InvokeDrop);
    CHECK_EQ(plan->steps.front().form, CaseTypeForm::Enumeration);
    const LirFunc &glue = LirFunction(package, plan->symbol);
    CHECK_EQ(Calls(glue, "Status::~Status"), 1);
    CHECK_EQ(Blocks(glue, "drop.variant"), 0);
    CHECK(std::ranges::none_of(glue.blocks, [](const LirBlock &block) {
        return std::ranges::any_of(block.instrs,
                                   [](const LirInstr &instruction) { return instruction.op == LirOpcode::IndexPtr; });
    }));
}

TEST_CASE("propagation rolls back every completed variant payload in reverse order") {
    const LirPackage package = DropLir(kOwner + R"(
        enum Error: uint8 { Bad = 1 }
        variant Result<T, E> { Success(T), Error(E) }
        variant Choice { Empty, Build(Handle, Handle, int32) }

        func Read(ok: bool) -> Result<int32, Error> {
            if ok { return Result::Success<int32, Error>(7i32); }
            return Result::Error<int32, Error>(Error::Bad);
        }

        func Build(ok: bool) -> Result<Choice, Error> {
            return Result::Success<Choice, Error>(
                Choice::Build(Handle { value: 1i32 }, Handle { value: 2i32 }, Read(ok)?)
            );
        }
    )");
    const auto handle = std::ranges::find(package.dropGlues, TypeRef::MakeNamed("Handle"), &DropGluePlan::type);
    REQUIRE(handle != package.dropGlues.end());
    CHECK_FALSE(handle->symbol.empty());
    CHECK_EQ(handle->steps.size(), 1);
    const LirFunc &build = LirFunction(package, "Build");
    CHECK_EQ(Calls(build, handle->symbol), 2);
    CHECK_EQ(Calls(build, "Handle::~Handle"), 0);
    CHECK_GE(build.blocks.size(), 4);
    CHECK(std::ranges::any_of(
        build.blocks, [](const LirBlock &block) { return block.term && block.term->kind == LirTermKind::Branch; }));
}

TEST_CASE("AArch64 receives the same active-case drop dispatch") {
    TargetContext target = TargetContext::CreateNative();
    target.os = Target::OS::Linux;
    target.arch = Target::Arch::AArch64;
    target.abi = Target::ABI::AAPCS64;
    target.default_cc = Target::CallingConv::AAPCS64;
    target.object_format = Target::ObjectFormat::ELF;
    target.cpu_features = Target::CpuFeature::NEON;
    const LirPackage package = DropLir(kOwner + R"(
        variant Held { Empty, Full(Handle) }
        func Use(value: Held) {}
    )",
                                       target);
    const auto plan = std::ranges::find(package.dropGlues, TypeRef::MakeNamed("Held"), &DropGluePlan::type);
    REQUIRE(plan != package.dropGlues.end());
    const LirFunc &glue = LirFunction(package, plan->symbol);
    CHECK_EQ(Blocks(glue, "drop.variant"), 2);
    CHECK_EQ(Calls(glue, "Handle::~Handle"), 1);
}
