#include "MoveConsumptionTestSupport.h"

using namespace Rux;
using namespace Rux::Testing::MoveConsumptionTestSupport;

TEST_CASE("named moves invoke recursive custom operations while temporaries transfer directly") {
    const std::string source = R"(

        struct Cell { value: int32; }
        extend Cell {
            func =(self: &var Cell, other: &Cell);
            func ~Cell(self: &var Cell) {}
            func <-(self: &var Cell, other: Cell) {
                self.value = other.value;
            }
        }

        struct Wrapper { cell: Cell; tag: int32; }

        func Move(source: Wrapper) -> Wrapper {
            let local <- source;
            return <- local;
        }
        func Fresh() -> Wrapper {
            return Wrapper { cell: Cell { value: 1 }, tag: 2 };
        }
    )";

    const HirPackage hir = LowerConsumptionHir(source);
    const HirFunc &move = RequireFunction(hir, "Move");
    REQUIRE(move.body.has_value());
    const auto *local = dynamic_cast<const HirLetStmt *>(move.body->stmts[0].get());
    const auto *returned = dynamic_cast<const HirReturnStmt *>(move.body->stmts[1].get());
    REQUIRE(local != nullptr);
    REQUIRE(returned != nullptr);
    REQUIRE(returned->value.has_value());
    const auto *localMove = dynamic_cast<const HirMoveExpr *>(local->init.get());
    const auto *returnMove = dynamic_cast<const HirMoveExpr *>((*returned->value).get());
    REQUIRE(localMove != nullptr);
    REQUIRE(returnMove != nullptr);
    CHECK_EQ(localMove->consumedBindingId, move.params[0].bindingId);
    CHECK_EQ(returnMove->consumedBindingId, local->bindingId);
    CHECK_EQ(localMove->plan.kind, HirMovePlan::Kind::Structure);
    REQUIRE_EQ(localMove->plan.components.size(), 2);
    CHECK_EQ(localMove->plan.components[0].kind, HirMovePlan::Kind::Custom);
    const std::string moveSymbol = localMove->plan.components[0].customCallee;
    CHECK_FALSE(moveSymbol.empty());
    CHECK_EQ(returnMove->plan.components[0].customCallee, moveSymbol);

    const HirFunc &fresh = RequireFunction(hir, "Fresh");
    REQUIRE(fresh.body.has_value());
    const auto *freshReturn = dynamic_cast<const HirReturnStmt *>(fresh.body->stmts[0].get());
    REQUIRE(freshReturn != nullptr);
    REQUIRE(freshReturn->value.has_value());
    CHECK(dynamic_cast<const HirMoveExpr *>((*freshReturn->value).get()) == nullptr);

    const LirPackage lir = LowerConsumptionLir(source);
    CHECK_EQ(LirCallCount(RequireLirFunction(lir, "Move"), moveSymbol), 2);
}

TEST_CASE("custom move assignment reuses conditional destination cleanup") {
    const std::string source = R"(
        struct Cell { value: int32; }
        extend Cell {
            func =(self: &var Cell, other: &Cell);
            func ~Cell(self: &var Cell) {}
            func <-(self: &var Cell, other: Cell) {
                self.value = other.value;
            }
        }

        func Replace(destination: Cell, source: Cell) {
            var target <- destination;
            target <- source;
        }
        func Initialize(source: Cell) {
            var destination: Cell;
            destination <- source;
        }
    )";

    const HirPackage hir = LowerConsumptionHir(source);
    for (const std::string name : {"Replace", "Initialize"}) {
        const HirFunc &function = RequireFunction(hir, name);
        REQUIRE(function.body.has_value());
        const auto *statement = dynamic_cast<const HirExprStmt *>(function.body->stmts[1].get());
        REQUIRE(statement != nullptr);
        const auto *assignment = dynamic_cast<const HirAssignExpr *>(statement->expr.get());
        REQUIRE(assignment != nullptr);
        REQUIRE(assignment->overwriteCleanup.has_value());
        CHECK_NE(assignment->overwriteCleanup->bindingId, 0);
        const auto *move = dynamic_cast<const HirMoveExpr *>(assignment->value.get());
        REQUIRE(move != nullptr);
        CHECK_EQ(move->plan.kind, HirMovePlan::Kind::Custom);
        CHECK_FALSE(move->plan.customCallee.empty());
    }
}

TEST_CASE("prohibited move operations reject explicit ownership transfer") {
    const std::vector<SemanticDiagnostic> diagnostics = AnalyzeConsumptionDiagnostics(R"(
        struct Pinned { value: int32; }
        extend Pinned {
            func =(self: &var Pinned, other: &Pinned);
            func <-(self: &var Pinned, other: Pinned);
        }

        func Transfer(source: Pinned) {
            let destination <- source;
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics[0].message, "moving type 'Pinned' is prohibited");
}

TEST_CASE("named by-value sources use recursive custom copies while temporaries transfer directly") {
    const std::string source = R"(

        struct Cell { value: int32; }
        extend Cell {
            func ~Cell(self: &var Cell) {}
            func =(self: &var Cell, other: &Cell) {
                self.value = other.value;
            }
        }

        struct Wrapper { cell: Cell; tag: int32; }

        func Make(value: int32) -> Wrapper {
            return Wrapper { cell: Cell { value: value }, tag: value };
        }
        func Take(value: Wrapper) {}
        func Clone(source: Wrapper) -> Wrapper {
            let local = source;
            Take(source);
            return source;
        }
        func Direct() -> Wrapper {
            return Make(1);
        }
    )";

    const HirPackage hir = LowerConsumptionHir(source);
    const HirFunc &clone = RequireFunction(hir, "Clone");
    REQUIRE(clone.body.has_value());
    const auto *local = dynamic_cast<const HirLetStmt *>(clone.body->stmts[0].get());
    const auto *takeStatement = dynamic_cast<const HirExprStmt *>(clone.body->stmts[1].get());
    const auto *returned = dynamic_cast<const HirReturnStmt *>(clone.body->stmts[2].get());
    REQUIRE(local != nullptr);
    REQUIRE(takeStatement != nullptr);
    REQUIRE(returned != nullptr);
    REQUIRE(returned->value.has_value());
    const auto *localCopy = dynamic_cast<const HirCopyExpr *>(local->init.get());
    const auto *take = dynamic_cast<const HirCallExpr *>(takeStatement->expr.get());
    const auto *returnCopy = dynamic_cast<const HirCopyExpr *>((*returned->value).get());
    REQUIRE(localCopy != nullptr);
    REQUIRE(take != nullptr);
    REQUIRE_EQ(take->args.size(), 1);
    const auto *argumentCopy = dynamic_cast<const HirCopyExpr *>(take->args.front().get());
    REQUIRE(argumentCopy != nullptr);
    REQUIRE(returnCopy != nullptr);
    CHECK_EQ(localCopy->plan.kind, HirCopyPlan::Kind::Structure);
    REQUIRE_EQ(localCopy->plan.components.size(), 2);
    CHECK_EQ(localCopy->plan.components[0].kind, HirCopyPlan::Kind::Custom);
    const std::string copySymbol = localCopy->plan.components[0].customCallee;
    CHECK_FALSE(copySymbol.empty());
    CHECK_EQ(argumentCopy->plan.components[0].customCallee, copySymbol);
    CHECK_EQ(returnCopy->plan.components[0].customCallee, copySymbol);

    const HirFunc &direct = RequireFunction(hir, "Direct");
    REQUIRE(direct.body.has_value());
    const auto *directReturn = dynamic_cast<const HirReturnStmt *>(direct.body->stmts[0].get());
    REQUIRE(directReturn != nullptr);
    REQUIRE(directReturn->value.has_value());
    CHECK(dynamic_cast<const HirCopyExpr *>((*directReturn->value).get()) == nullptr);

    const LirPackage lir = LowerConsumptionLir(source);
    CHECK_EQ(LirCallCount(RequireLirFunction(lir, "Clone"), copySymbol), 3);
}

TEST_CASE("cross-source copy assignment constructs scratch before replacing the destination") {
    const std::string source = R"(

        struct Seed { value: int32; }
        struct Cell { value: int32; }
        extend Cell {
            func ~Cell(self: &var Cell) {}
            func =(self: &var Cell, other: &Cell) {
                self.value = other.value;
            }
            func =(self: &var Cell, other: &Seed) {
                self.value = other.value;
            }
        }

        func Replace(source: Seed) {
            var destination = Cell { value: 0 };
            destination = source;
        }
    )";

    const HirPackage hir = LowerConsumptionHir(source);
    const HirFunc &replace = RequireFunction(hir, "Replace");
    REQUIRE(replace.body.has_value());
    const auto *statement = dynamic_cast<const HirExprStmt *>(replace.body->stmts[1].get());
    REQUIRE(statement != nullptr);
    const auto *assignment = dynamic_cast<const HirAssignExpr *>(statement->expr.get());
    REQUIRE(assignment != nullptr);
    REQUIRE(assignment->overwriteCleanup.has_value());
    const auto *copy = dynamic_cast<const HirCopyExpr *>(assignment->value.get());
    REQUIRE(copy != nullptr);
    CHECK_EQ(copy->type, TypeRef::MakeNamed("Cell"));
    CHECK_EQ(copy->value->type, TypeRef::MakeNamed("Seed"));
    CHECK_EQ(copy->plan.kind, HirCopyPlan::Kind::Custom);
    CHECK_FALSE(copy->plan.customCallee.empty());

    const LirPackage lir = LowerConsumptionLir(source);
    CHECK_EQ(LirCallCount(RequireLirFunction(lir, "Replace"), copy->plan.customCallee), 1);
}

TEST_CASE("generated variant copies recursively copy the active owning payload") {
    const std::string source = R"(

        struct Cell { value: int32; }
        extend Cell {
            func ~Cell(self: &var Cell) {}
            func =(self: &var Cell, other: &Cell) {
                self.value = other.value;
            }
        }

        variant Maybe<T> { Some(T), None }

        func Take(value: Maybe<Cell>) {}
        func Clone(source: Maybe<Cell>) {
            Take(source);
        }
    )";

    const HirPackage hir = LowerConsumptionHir(source);
    const HirFunc &clone = RequireFunction(hir, "Clone");
    REQUIRE(clone.body.has_value());
    const auto *statement = dynamic_cast<const HirExprStmt *>(clone.body->stmts[0].get());
    REQUIRE(statement != nullptr);
    const auto *call = dynamic_cast<const HirCallExpr *>(statement->expr.get());
    REQUIRE(call != nullptr);
    REQUIRE_EQ(call->args.size(), 1);
    const auto *copy = dynamic_cast<const HirCopyExpr *>(call->args.front().get());
    REQUIRE(copy != nullptr);
    CHECK_EQ(copy->plan.kind, HirCopyPlan::Kind::Enum);
    CHECK_EQ(copy->plan.form, CaseTypeForm::Variant);
    REQUIRE_EQ(copy->plan.variantComponents.size(), 2);
    REQUIRE_EQ(copy->plan.variantComponents[0].size(), 1);
    CHECK_EQ(copy->plan.variantComponents[0][0].kind, HirCopyPlan::Kind::Custom);
    const std::string copySymbol = copy->plan.variantComponents[0][0].customCallee;
    CHECK_FALSE(copySymbol.empty());

    const LirPackage lir = LowerConsumptionLir(source);
    CHECK_EQ(LirCallCount(RequireLirFunction(lir, "Clone"), copySymbol), 1);
}

TEST_CASE("variant move plans dispatch custom moves for only the active case") {
    const std::string source = R"(
        struct Handle { value: int32; }
        extend Handle {
            func =(self: &var Handle, other: &Handle);
            func <-(self: &var Handle, other: Handle) { self.value = other.value; }
            func ~Handle(self: &var Handle) {}
        }

        variant Choice {
            Empty,
            Direct(Handle),
            Pair(int32, Handle),
            Named { sequence: uint64; handle: Handle; }
        }

        func Take(value: Choice) {}
        func Transfer(source: Choice) {
            let moved <- source;
            Take(<-moved);
        }
    )";

    const HirPackage hir = LowerConsumptionHir(source);
    const HirFunc &transfer = RequireFunction(hir, "Transfer");
    REQUIRE(transfer.body.has_value());
    REQUIRE_GE(transfer.body->stmts.size(), 2);
    const auto *binding = dynamic_cast<const HirLetStmt *>(transfer.body->stmts[0].get());
    REQUIRE(binding != nullptr);
    const auto *move = dynamic_cast<const HirMoveExpr *>(binding->init.get());
    REQUIRE(move != nullptr);
    CHECK_EQ(move->plan.kind, HirMovePlan::Kind::Variant);
    CHECK_EQ(move->plan.form, CaseTypeForm::Variant);
    CHECK_EQ(move->plan.variantDiscriminants, std::vector<std::string>{"0", "1", "2", "3"});
    REQUIRE_EQ(move->plan.variantPayloadTypes.size(), 4);
    CHECK(move->plan.variantPayloadTypes[0].empty());
    CHECK_EQ(move->plan.variantPayloadTypes[1], std::vector<TypeRef>{TypeRef::MakeNamed("Handle")});
    CHECK_EQ(move->plan.variantPayloadTypes[2],
             std::vector<TypeRef>{TypeRef::MakeInt32(), TypeRef::MakeNamed("Handle")});
    CHECK_EQ(move->plan.variantPayloadTypes[3],
             std::vector<TypeRef>{TypeRef::MakeUInt64(), TypeRef::MakeNamed("Handle")});
    REQUIRE_EQ(move->plan.variantComponents.size(), 4);
    CHECK(move->plan.variantComponents[0].empty());
    REQUIRE_EQ(move->plan.variantComponents[1].size(), 1);
    CHECK_EQ(move->plan.variantComponents[1][0].kind, HirMovePlan::Kind::Custom);
    REQUIRE_EQ(move->plan.variantComponents[2].size(), 2);
    CHECK_EQ(move->plan.variantComponents[2][0].kind, HirMovePlan::Kind::Trivial);
    CHECK_EQ(move->plan.variantComponents[2][1].kind, HirMovePlan::Kind::Custom);
    REQUIRE_EQ(move->plan.variantComponents[3].size(), 2);
    CHECK_EQ(move->plan.variantComponents[3][1].kind, HirMovePlan::Kind::Custom);
    const std::string moveSymbol = move->plan.variantComponents[1][0].customCallee;
    CHECK_FALSE(moveSymbol.empty());

    const LirPackage lir = LowerConsumptionLir(source);
    const LirFunc &lowered = RequireLirFunction(lir, "Transfer");
    CHECK_GE(LirCallCount(lowered, moveSymbol), 3);
    CHECK_GE(std::ranges::count_if(lowered.blocks,
                                   [](const LirBlock &block) { return block.label.starts_with("move.variant"); }),
             3);
}

TEST_CASE("nested generic variant move plans preserve substituted active-case structure") {
    const std::string source = R"(
        struct Handle { value: int32; }
        extend Handle {
            func =(self: &var Handle, other: &Handle);
            func <-(self: &var Handle, other: Handle) { self.value = other.value; }
            func ~Handle(self: &var Handle) {}
        }
        variant Maybe<T> { None, Some(T) }
        variant Envelope<T> { Empty, Direct(T), Nested(Maybe<T>) }
        func Transfer(source: Envelope<Handle>) {
            let moved <- source;
        }
    )";

    const HirPackage hir = LowerConsumptionHir(source);
    const HirFunc &transfer = RequireFunction(hir, "Transfer");
    REQUIRE(transfer.body.has_value());
    const auto *binding = dynamic_cast<const HirLetStmt *>(transfer.body->stmts.front().get());
    REQUIRE(binding != nullptr);
    const auto *move = dynamic_cast<const HirMoveExpr *>(binding->init.get());
    REQUIRE(move != nullptr);
    CHECK_EQ(move->plan.kind, HirMovePlan::Kind::Variant);
    REQUIRE_EQ(move->plan.variantPayloadTypes.size(), 3);
    CHECK_EQ(move->plan.variantPayloadTypes[1], std::vector<TypeRef>{TypeRef::MakeNamed("Handle")});
    CHECK_EQ(move->plan.variantPayloadTypes[2], std::vector<TypeRef>{TypeRef::MakeNamed("Maybe<Handle>")});
    REQUIRE_EQ(move->plan.variantComponents[2].size(), 1);
    CHECK_EQ(move->plan.variantComponents[2][0].kind, HirMovePlan::Kind::Variant);
    CHECK_EQ(move->plan.variantComponents[2][0].form, CaseTypeForm::Variant);
}

TEST_CASE("scalar enums remain trivial copies and never acquire variant lifecycle plans") {
    const std::string source = R"(
        enum Status: uint8 { Ready = 1, Busy = 2 }
        func Take(value: Status) {}
        func Copy(source: Status) -> Status {
            let local = source;
            Take(source);
            return local;
        }
    )";

    const HirPackage hir = LowerConsumptionHir(source);
    const HirFunc &copy = RequireFunction(hir, "Copy");
    REQUIRE(copy.body.has_value());
    const auto *binding = dynamic_cast<const HirLetStmt *>(copy.body->stmts[0].get());
    REQUIRE(binding != nullptr);
    CHECK(dynamic_cast<const HirCopyExpr *>(binding->init.get()) == nullptr);
    const LirPackage lir = LowerConsumptionLir(source);
    const LirFunc &lowered = RequireLirFunction(lir, "Copy");
    CHECK_FALSE(std::ranges::any_of(lowered.blocks, [](const LirBlock &block) {
        return block.label.starts_with("copy.variant") || block.label.starts_with("move.variant");
    }));
    CHECK(AnalyzeConsumptionDiagnostics(source).empty());
}

TEST_CASE("consuming a move-only variant reports later use while borrowed inspection remains reusable") {
    const std::vector<SemanticDiagnostic> diagnostics = AnalyzeConsumptionDiagnostics(R"(
        struct Handle { value: int32; }
        extend Handle {
            func =(self: &var Handle, other: &Handle);
            func ~Handle(self: &var Handle) {}
        }
        variant Held { Empty, Full(Handle) }

        func Consume(value: Held) {
            match <-value { .Empty => {}, .Full(handle) => {} }
            value;
        }

        func Inspect(value: *Held) {
            match *value { .Empty => {}, .Full(handle) => {} }
            match *value { .Empty => {}, .Full(_) => {} }
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics.front().message, "value 'value' is used after it was moved");
}
