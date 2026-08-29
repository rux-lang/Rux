#include "Ir/Lir/Lir.h"
#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <doctest.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace Rux;

namespace {
std::vector<SemanticDiagnostic> AnalyzeConsumptionDiagnostics(const std::string &source) {
    Lexer lexer(source, "move_consumption.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "move_consumption.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    return analyzer.Analyze().diagnostics;
}

HirPackage LowerConsumptionHir(const std::string &source) {
    Lexer lexer(source, "move_cleanup.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "move_cleanup.rux");
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

LirPackage LowerConsumptionLir(const std::string &source) {
    Lexer lexer(source, "copy_lowering.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "copy_lowering.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", CompileTimeContext{});
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());
    HirToLirLowering lowering(AstToHirLowering(model).Generate(), CompileTimeContext{}.target);
    LirPackage package = lowering.Generate();
    REQUIRE(lowering.Diagnostics().empty());
    return package;
}

const LirFunc &RequireLirFunction(const LirPackage &package, const std::string &name) {
    for (const LirModule &module : package.modules) {
        for (const LirFunc &function : module.funcs) {
            if (function.name == name) {
                return function;
            }
        }
    }
    FAIL("missing lowered LIR function " << name);
    throw std::runtime_error("missing lowered LIR function");
}

std::size_t LirCallCount(const LirFunc &function, const std::string &symbol) {
    std::size_t count = 0;
    for (const LirBlock &block : function.blocks) {
        for (const LirInstr &instruction : block.instrs) {
            if (instruction.op == LirOpcode::Call && instruction.strArg == symbol) {
                ++count;
            }
        }
    }
    return count;
}
} // namespace

TEST_CASE("explicit move syntax parses in bindings assignments calls and returns") {
    Lexer lexer(R"(
        func Take(value: int32) -> int32 { return value; }
        func Transfer(source: int32) -> int32 {
            let bound <- source;
            var destination = 0;
            destination <- bound;
            return Take(<- destination);
        }
    )",
                "explicit_move_syntax.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "explicit_move_syntax.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    const auto *transfer = dynamic_cast<const FuncDecl *>(parsed.module.items[1].get());
    REQUIRE(transfer != nullptr);
    REQUIRE(transfer->body != nullptr);
    REQUIRE_EQ(transfer->body->stmts.size(), 4);

    const auto *bound = dynamic_cast<const LetStmt *>(transfer->body->stmts[0].get());
    REQUIRE(bound != nullptr);
    const auto *bindingMove = dynamic_cast<const MoveExpr *>(bound->init.get());
    REQUIRE(bindingMove != nullptr);
    CHECK(dynamic_cast<const IdentExpr *>(bindingMove->operand.get()) != nullptr);

    const auto *assignmentStatement = dynamic_cast<const ExprStmt *>(transfer->body->stmts[2].get());
    REQUIRE(assignmentStatement != nullptr);
    const auto *assignment = dynamic_cast<const AssignExpr *>(assignmentStatement->expr.get());
    REQUIRE(assignment != nullptr);
    CHECK_EQ(assignment->op, TokenKind::MoveArrow);

    const auto *returned = dynamic_cast<const ReturnStmt *>(transfer->body->stmts[3].get());
    REQUIRE(returned != nullptr);
    REQUIRE(returned->value.has_value());
    const auto *call = dynamic_cast<const CallExpr *>((*returned->value).get());
    REQUIRE(call != nullptr);
    REQUIRE_EQ(call->args.size(), 1);
    CHECK(dynamic_cast<const MoveExpr *>(call->args[0].get()) != nullptr);
}

TEST_CASE("explicit moves invalidate copyable places and reject invalid ownership sources") {
    const std::vector<SemanticDiagnostic> diagnostics = AnalyzeConsumptionDiagnostics(R"(
        struct Pair { first: int32; second: int32; }
        struct Handle { value: int32; }
        extend Handle {
            func =(self: &var Handle, other: &Handle);
            func ~Handle(self: &var Handle) {}
        }

        func ThroughPointer(pointer: *Pair) {
            let pointee <- *pointer;
        }

        func ThroughReference(pair: &Pair) {
            let field <- pair.first;
        }

        func MoveReference(pair: &Pair) {
            let alias <- pair;
        }

        func AssignReference(pair: &Pair, other: &Pair) {
            var alias: &Pair = pair;
            alias <- other;
        }

        func MoveBorrowed() {
            let pair = Pair { first: 1, second: 2 };
            let alias: &Pair = pair;
            let moved <- pair;
            alias.first;
        }

        func MoveBorrowedHandle() {
            let handle = Handle { value: 1 };
            let alias: &Handle = handle;
            let moved <- handle;
            alias.value;
        }

        func Main() {
            let source = 1;
            let destination <- source;
            source;

            var selfMove = 2;
            selfMove <- selfMove;

            let pair = Pair { first: 3, second: 4 };
            let partial <- pair.first;
        }
    )");

    const std::vector<std::string> expected = {
        "cannot move '*pointer' out of borrowed pointer storage",
        "cannot move 'pair.first' out of borrowed reference storage",
        "cannot move a non-owning reference",
        "cannot move ownership into a reference",
        "cannot move 'pair' while it is immutably borrowed",
        "cannot move 'handle' while it is immutably borrowed",
        "value 'source' is used after it was moved",
        "cannot move 'selfMove' into itself",
        "cannot move field 'first' out of droppable value 'pair'",
    };
    REQUIRE_EQ(diagnostics.size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        CHECK_EQ(diagnostics[index].message, expected[index]);
    }
}

TEST_CASE("explicit moves retain source drop-flag transfers in HIR") {
    const HirPackage package = LowerConsumptionHir(R"(
        struct Handle { value: int32; }
        extend Handle {
            func =(self: &var Handle, other: &Handle);
            func ~Handle(self: &var Handle) {}
        }

        func Transfer(source: Handle) -> Handle {
            let local <- source;
            return <- local;
        }

        func Replace(destination: Handle, source: Handle) {
            var target <- destination;
            target <- source;
        }

        func Fresh() -> Handle {
            return Handle { value: 1 };
        }
    )");

    const HirFunc &transfer = RequireFunction(package, "Transfer");
    REQUIRE(transfer.body.has_value());
    REQUIRE_EQ(transfer.params.size(), 1);
    const auto *local = dynamic_cast<const HirLetStmt *>(transfer.body->stmts[0].get());
    const auto *returned = dynamic_cast<const HirReturnStmt *>(transfer.body->stmts[1].get());
    REQUIRE(local != nullptr);
    REQUIRE(local->init != nullptr);
    REQUIRE(returned != nullptr);
    REQUIRE(returned->value.has_value());
    CHECK_EQ(local->init->consumption, ValueConsumptionKind::ExplicitMove);
    CHECK_EQ(local->init->consumedBindingId, transfer.params[0].bindingId);
    CHECK_EQ((*returned->value)->consumption, ValueConsumptionKind::ExplicitMove);
    CHECK_EQ((*returned->value)->consumedBindingId, local->bindingId);

    const HirFunc &replace = RequireFunction(package, "Replace");
    REQUIRE(replace.body.has_value());
    REQUIRE_EQ(replace.params.size(), 2);
    const auto *target = dynamic_cast<const HirLetStmt *>(replace.body->stmts[0].get());
    const auto *statement = dynamic_cast<const HirExprStmt *>(replace.body->stmts[1].get());
    REQUIRE(target != nullptr);
    REQUIRE(statement != nullptr);
    const auto *assignment = dynamic_cast<const HirAssignExpr *>(statement->expr.get());
    REQUIRE(assignment != nullptr);
    CHECK_EQ(assignment->op, TokenKind::MoveArrow);
    REQUIRE(assignment->overwriteCleanup.has_value());
    CHECK_EQ(assignment->overwriteCleanup->bindingId, target->bindingId);
    CHECK_EQ(assignment->value->consumption, ValueConsumptionKind::ExplicitMove);
    CHECK_EQ(assignment->value->consumedBindingId, replace.params[1].bindingId);

    const HirFunc &fresh = RequireFunction(package, "Fresh");
    REQUIRE(fresh.body.has_value());
    const auto *freshReturn = dynamic_cast<const HirReturnStmt *>(fresh.body->stmts[0].get());
    REQUIRE(freshReturn != nullptr);
    REQUIRE(freshReturn->value.has_value());
    CHECK_EQ((*freshReturn->value)->consumption, ValueConsumptionKind::Return);
    CHECK_EQ((*freshReturn->value)->consumedBindingId, 0);
}

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

TEST_CASE("named move-only values require explicit transfer syntax in every by-value context") {
    const std::vector<SemanticDiagnostic> diagnostics = AnalyzeConsumptionDiagnostics(R"(

        struct Handle { value: int32; }
        extend Handle {
            func =(self: &var Handle, other: &Handle);
            func ~Handle(self: &var Handle) {}
        }

        func Take(value: Handle) {}

        func MissingInitialization(source: Handle) {
            let destination = source;
        }

        func MissingArgument(source: Handle) {
            Take(source);
        }

        func MissingReturn(source: Handle) -> Handle {
            return source;
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 3);
    CHECK_EQ(diagnostics[0].message, "move-only value 'source' requires an explicit '<-' in initialization");
    CHECK_EQ(diagnostics[0].help, "write 'let destination <- source' to transfer ownership");
    CHECK_EQ(diagnostics[1].message, "move-only value 'source' requires an explicit '<-' in argument");
    CHECK_EQ(diagnostics[1].help, "prefix the argument with '<-', as in 'Take(<-source)'");
    CHECK_EQ(diagnostics[2].message, "move-only value 'source' requires an explicit '<-' in return");
    CHECK_EQ(diagnostics[2].help, "prefix the return value with '<-', as in 'return <-source'");
}

TEST_CASE("explicit syntax consumes move-only values in every by-value context") {
    const std::vector<SemanticDiagnostic> diagnostics = AnalyzeConsumptionDiagnostics(R"(

        struct Handle { value: int32; }
        extend Handle {
            func =(self: &var Handle, other: &Handle);
            func ~Handle(self: &var Handle) {}
        }
        extend Handle {
            func Consume(self: Handle) {}
        }
        struct Owner { handle: Handle; }

        func NewHandle(value: int32) -> Handle {
            return Handle { value: value };
        }

        func Take(value: Handle) {}

        func Main(flag: bool) {
            let initialized = NewHandle(1);
            let movedByInitialization <- initialized;
            initialized;

            var assigned = NewHandle(2);
            var destination = NewHandle(3);
            destination <- assigned;
            assigned;

            let argument = NewHandle(4);
            Take(<-argument);
            argument;

            let field = NewHandle(5);
            let owner = Owner { handle: <-field };
            field;

            let arrayValue = NewHandle(6);
            let values = [<-arrayValue];
            arrayValue;

            let tupleValue = NewHandle(7);
            let tuple = (<-tupleValue, 1);
            tupleValue;

            let receiver = NewHandle(8);
            (<-receiver).Consume();
            receiver;

            var ternaryLeft = NewHandle(9);
            var ternaryRight = NewHandle(10);
            let selected = flag ? <-ternaryLeft : <-ternaryRight;
            ternaryLeft;
            ternaryRight;

            var reusable = NewHandle(11);
            let consumed <- reusable;
            reusable <- NewHandle(12);
            Take(<-reusable);
            reusable;

            let copy = 12;
            let copied = copy;
            copy;
        }
    )");

    const std::vector<std::string> expected = {
        "value 'initialized' is used after it was moved",
        "value 'assigned' is used after it was moved",
        "value 'argument' is used after it was moved",
        "value 'field' is used after it was moved",
        "value 'arrayValue' is used after it was moved",
        "value 'tupleValue' is used after it was moved",
        "value 'receiver' is used after it was moved",
        "value 'ternaryLeft' may have been moved on some control-flow paths",
        "value 'ternaryRight' may have been moved on some control-flow paths",
        "value 'reusable' is used after it was moved",
    };
    REQUIRE_EQ(diagnostics.size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        CHECK_EQ(diagnostics[index].message, expected[index]);
    }
}

TEST_CASE("accepted ownership transfers are retained as semantic and HIR facts") {
    Lexer lexer(R"(
        struct Handle { value: int32; }
        extend Handle {
            func =(self: &var Handle, other: &Handle);
            func ~Handle(self: &var Handle) {}
        }

        func ReturnHandle(value: Handle) -> Handle {
            return <-value;
        }

        func Main() {
            let first = Handle { value: 1 };
            let second <- first;
            ReturnHandle(<-second);
        }
    )",
                "move_consumption_facts.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "move_consumption_facts.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());

    const auto *returnFunction = dynamic_cast<const FuncDecl *>(parsed.module.items[2].get());
    const auto *returnStatement = dynamic_cast<const ReturnStmt *>(returnFunction->body->stmts[0].get());
    REQUIRE(returnStatement != nullptr);
    const Expr &returned = **returnStatement->value;
    const auto *returnedMove = dynamic_cast<const MoveExpr *>(&returned);
    REQUIRE(returnedMove != nullptr);
    const ValueConsumption *returnFact = model.TryGetConsumption(*returnedMove->operand);
    REQUIRE(returnFact != nullptr);
    CHECK_EQ(returnFact->kind, ValueConsumptionKind::ExplicitMove);
    CHECK_EQ(returnFact->type, TypeRef::MakeNamed("Handle"));

    const auto *mainFunction = dynamic_cast<const FuncDecl *>(parsed.module.items[3].get());
    const auto *second = dynamic_cast<const LetStmt *>(mainFunction->body->stmts[1].get());
    REQUIRE(second != nullptr);
    const auto *initializationMove = dynamic_cast<const MoveExpr *>(second->init.get());
    REQUIRE(initializationMove != nullptr);
    const ValueConsumption *initializationFact = model.TryGetConsumption(*initializationMove->operand);
    REQUIRE(initializationFact != nullptr);
    CHECK_EQ(initializationFact->kind, ValueConsumptionKind::ExplicitMove);

    const HirPackage package = AstToHirLowering(model).Generate();
    REQUIRE_EQ(package.modules.size(), 1);
    REQUIRE_EQ(package.modules[0].funcs.size(), 2);
    const HirFunc &loweredMain = package.modules[0].funcs[1];
    REQUIRE(loweredMain.body.has_value());
    const auto *loweredSecond = dynamic_cast<const HirLetStmt *>(loweredMain.body->stmts[1].get());
    REQUIRE(loweredSecond != nullptr);
    REQUIRE(loweredSecond->init->consumption.has_value());
    CHECK_EQ(*loweredSecond->init->consumption, ValueConsumptionKind::ExplicitMove);
}

TEST_CASE("an explicit generic store retains ownership transfer for its instantiation") {
    // `<-` states that the generic value is transferred. The record is keyed by the generic expression that every
    // instantiation shares, so it keeps the unsubstituted type: each instantiation substitutes its own type argument
    // when lowering builds the move plan. Recording one instantiation's concrete type here would hand that type's
    // move operation to every other instantiation of the same body.
    Lexer lexer(R"(
        struct Handle { value: int32; }
        extend Handle {
            func =(self: &var Handle, other: &Handle);
            func ~Handle(self: &var Handle) {}
        }

        func Store<T>(slot: *var T, value: T) {
            *slot <- value;
        }

        func Main() {
            var room = Handle { value: 0 };
            Store<Handle>(@room, Handle { value: 1 });
        }
    )",
                "generic_consumption.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "generic_consumption.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());

    const auto *store = dynamic_cast<const FuncDecl *>(parsed.module.items[2].get());
    REQUIRE(store != nullptr);
    const auto *statement = dynamic_cast<const ExprStmt *>(store->body->stmts[0].get());
    REQUIRE(statement != nullptr);
    const auto *assignment = dynamic_cast<const AssignExpr *>(statement->expr.get());
    REQUIRE(assignment != nullptr);

    const ValueConsumption *fact = model.TryGetConsumption(*assignment->value);
    REQUIRE(fact != nullptr);
    CHECK_EQ(fact->kind, ValueConsumptionKind::ExplicitMove);
    CHECK_EQ(fact->type, TypeRef::MakeTypeParam("T"));
    CHECK_EQ(fact->customOperation, nullptr);
}

TEST_CASE("an explicit move into a match consumes its subject") {
    // Taking a payload out of an option and destroying the option as well would destroy the payload twice, which is
    // what every unwrap in the standard packages did. Only a subject that is a value in its own right is consumed:
    // one read through a borrow has nothing taken from it, and a pattern that binds nothing takes nothing.
    Lexer lexer(R"(
        struct Handle { value: int32; }
        extend Handle {
            func =(self: &var Handle, other: &Handle);
            func ~Handle(self: &var Handle) {}
        }
        variant Held { Full(Handle), Empty }

        func Discard(held: Held) {
            match <-held {
                .Full(handle) => {},
                .Empty => {}
            }
        }

        func LookOnly(held: Held) {
            match held {
                .Full(_) => {},
                .Empty => {}
            }
        }

        func Borrowed(held: *Held) {
            match *held {
                .Full(handle) => {},
                .Empty => {}
            }
        }
    )",
                "match_consumption.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "match_consumption.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());

    const auto subjectOf = [&](const std::size_t item) -> const Expr & {
        const auto *function = dynamic_cast<const FuncDecl *>(parsed.module.items[item].get());
        REQUIRE(function != nullptr);
        const auto *statement = dynamic_cast<const MatchStmt *>(function->body->stmts[0].get());
        REQUIRE(statement != nullptr);
        return *statement->subject;
    };

    const auto *movedSubject = dynamic_cast<const MoveExpr *>(&subjectOf(3));
    REQUIRE(movedSubject != nullptr);
    const ValueConsumption *taken = model.TryGetConsumption(*movedSubject->operand);
    REQUIRE(taken != nullptr);
    CHECK_EQ(taken->kind, ValueConsumptionKind::ExplicitMove);
    CHECK(model.TryGetConsumption(subjectOf(4)) == nullptr);
    CHECK(model.TryGetConsumption(subjectOf(5)) == nullptr);
}

TEST_CASE("rejected by-value contexts do not move their operands") {
    const std::vector<SemanticDiagnostic> diagnostics = AnalyzeConsumptionDiagnostics(R"(
        struct Handle { value: int32; }
        extend Handle {
            func =(self: &var Handle, other: &Handle);
            func ~Handle(self: &var Handle) {}
        }
        struct Number { value: int32; }

        func Take(value: int32) {}

        func Main() {
            let assignment = Handle { value: 1 };
            let invalid: int32 = assignment;
            assignment;

            let argument = Handle { value: 2 };
            Take(argument);
            argument;

            let field = Handle { value: 3 };
            let invalidOwner = Number { value: field };
            field;
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 3);
    for (const SemanticDiagnostic &diagnostic : diagnostics) {
        CHECK(diagnostic.message.find("used after it was moved") == std::string::npos);
    }
}

TEST_CASE("ordinary scope exits destroy live bindings in reverse declaration order") {
    const HirPackage package = LowerConsumptionHir(R"(
        struct Handle { value: int32; }
        extend Handle {
            func =(self: &var Handle, other: &Handle);
            func ~Handle(self: &var Handle) {}
        }

        func Take(value: Handle) {}

        func Finish(parameter: Handle) {
            let first = Handle { value: 1 };
            if true {
                let first = Handle { value: 2 };
                first;
            }
            let last = Handle { value: 3 };
            Take(<-last);
        }
    )");

    const HirFunc &finish = RequireFunction(package, "Finish");
    REQUIRE(finish.body.has_value());
    REQUIRE_EQ(finish.params.size(), 1);
    CHECK_NE(finish.params[0].bindingId, 0);

    const HirBlock &body = *finish.body;
    REQUIRE_EQ(body.stmts.size(), 7);
    const auto *first = dynamic_cast<const HirLetStmt *>(body.stmts[0].get());
    const auto *nestedStatement = dynamic_cast<const HirIfStmt *>(body.stmts[1].get());
    const auto *last = dynamic_cast<const HirLetStmt *>(body.stmts[2].get());
    const auto *take = dynamic_cast<const HirExprStmt *>(body.stmts[3].get());
    REQUIRE(first != nullptr);
    REQUIRE(nestedStatement != nullptr);
    REQUIRE(last != nullptr);
    REQUIRE(take != nullptr);

    REQUIRE_EQ(nestedStatement->thenBlock.stmts.size(), 3);
    const auto *nestedLet = dynamic_cast<const HirLetStmt *>(nestedStatement->thenBlock.stmts[0].get());
    const auto *nestedDrop = dynamic_cast<const HirDropStmt *>(nestedStatement->thenBlock.stmts[2].get());
    REQUIRE(nestedLet != nullptr);
    REQUIRE(nestedDrop != nullptr);
    CHECK_EQ(nestedLet->name, first->name);
    CHECK_NE(nestedLet->bindingId, first->bindingId);
    CHECK_EQ(nestedDrop->action.bindingId, nestedLet->bindingId);
    CHECK_EQ(nestedDrop->action.origin.line, nestedLet->location.line);

    const auto *dropLast = dynamic_cast<const HirDropStmt *>(body.stmts[4].get());
    const auto *dropFirst = dynamic_cast<const HirDropStmt *>(body.stmts[5].get());
    const auto *dropParameter = dynamic_cast<const HirDropStmt *>(body.stmts[6].get());
    REQUIRE(dropLast != nullptr);
    REQUIRE(dropFirst != nullptr);
    REQUIRE(dropParameter != nullptr);
    CHECK_EQ(dropLast->action.bindingId, last->bindingId);
    CHECK_EQ(dropFirst->action.bindingId, first->bindingId);
    CHECK_EQ(dropParameter->action.bindingId, finish.params[0].bindingId);

    const auto *call = dynamic_cast<const HirCallExpr *>(take->expr.get());
    REQUIRE(call != nullptr);
    REQUIRE_EQ(call->args.size(), 1);
    CHECK_EQ(call->args[0]->consumedBindingId, last->bindingId);
    CHECK_EQ(dropLast->action.bindingId, call->args[0]->consumedBindingId);
}

TEST_CASE("returns evaluate their value before conditionally destroying every live scope") {
    const HirPackage package = LowerConsumptionHir(R"(
        struct Handle { value: int32; }
        extend Handle {
            func =(self: &var Handle, other: &Handle);
            func ~Handle(self: &var Handle) {}
        }

        func Choose(parameter: Handle, selectNested: bool) -> Handle {
            let outer = Handle { value: 1 };
            if selectNested {
                let nested = Handle { value: 2 };
                return <-nested;
            }
            return <-parameter;
        }
    )");

    const HirFunc &choose = RequireFunction(package, "Choose");
    REQUIRE(choose.body.has_value());
    REQUIRE_EQ(choose.params.size(), 2);
    const HirBlock &body = *choose.body;
    REQUIRE_EQ(body.stmts.size(), 5);
    const auto *outer = dynamic_cast<const HirLetStmt *>(body.stmts[0].get());
    const auto *conditional = dynamic_cast<const HirIfStmt *>(body.stmts[1].get());
    const auto *returnParameter = dynamic_cast<const HirReturnStmt *>(body.stmts[2].get());
    REQUIRE(outer != nullptr);
    REQUIRE(conditional != nullptr);
    REQUIRE(returnParameter != nullptr);

    REQUIRE_EQ(conditional->thenBlock.stmts.size(), 3);
    const auto *nested = dynamic_cast<const HirLetStmt *>(conditional->thenBlock.stmts[0].get());
    const auto *returnNested = dynamic_cast<const HirReturnStmt *>(conditional->thenBlock.stmts[1].get());
    REQUIRE(nested != nullptr);
    REQUIRE(returnNested != nullptr);
    REQUIRE(returnNested->value.has_value());
    REQUIRE_EQ(returnNested->cleanups.size(), 3);
    CHECK_EQ(returnNested->cleanups[0].bindingId, nested->bindingId);
    CHECK_EQ(returnNested->cleanups[1].bindingId, outer->bindingId);
    CHECK_EQ(returnNested->cleanups[2].bindingId, choose.params[0].bindingId);
    CHECK_EQ((*returnNested->value)->consumedBindingId, nested->bindingId);

    REQUIRE(returnParameter->value.has_value());
    REQUIRE_EQ(returnParameter->cleanups.size(), 2);
    CHECK_EQ(returnParameter->cleanups[0].bindingId, outer->bindingId);
    CHECK_EQ(returnParameter->cleanups[1].bindingId, choose.params[0].bindingId);
    CHECK_EQ((*returnParameter->value)->consumedBindingId, choose.params[0].bindingId);
    CHECK_NE(nested->bindingId, outer->bindingId);
    CHECK_NE(outer->bindingId, choose.params[0].bindingId);
}

TEST_CASE("loop exits carry every cleanup between the statement and its selected loop boundary") {
    const HirPackage package = LowerConsumptionHir(R"(
        struct Handle { value: int32; }
        extend Handle {
            func =(self: &var Handle, other: &Handle);
            func ~Handle(self: &var Handle) {}
        }

        func Run(flag: bool) {
            outer: loop {
                let outerValue = Handle { value: 1 };
                inner: loop {
                    let innerValue = Handle { value: 2 };
                    if flag { break outer; }
                    continue inner;
                }
            }
        }
    )");

    const HirFunc &run = RequireFunction(package, "Run");
    REQUIRE(run.body.has_value());
    const auto *outer = dynamic_cast<const HirLoopStmt *>(run.body->stmts[0].get());
    REQUIRE(outer != nullptr);
    const auto *outerValue = dynamic_cast<const HirLetStmt *>(outer->body.stmts[0].get());
    const auto *inner = dynamic_cast<const HirLoopStmt *>(outer->body.stmts[1].get());
    REQUIRE(outerValue != nullptr);
    REQUIRE(inner != nullptr);
    const auto *innerValue = dynamic_cast<const HirLetStmt *>(inner->body.stmts[0].get());
    const auto *conditional = dynamic_cast<const HirIfStmt *>(inner->body.stmts[1].get());
    const auto *continued = dynamic_cast<const HirContinueStmt *>(inner->body.stmts[2].get());
    REQUIRE(innerValue != nullptr);
    REQUIRE(conditional != nullptr);
    REQUIRE(continued != nullptr);

    REQUIRE_EQ(conditional->thenBlock.stmts.size(), 1);
    const auto *broken = dynamic_cast<const HirBreakStmt *>(conditional->thenBlock.stmts[0].get());
    REQUIRE(broken != nullptr);
    REQUIRE_EQ(broken->cleanups.size(), 2);
    CHECK_EQ(broken->cleanups[0].bindingId, innerValue->bindingId);
    CHECK_EQ(broken->cleanups[1].bindingId, outerValue->bindingId);

    REQUIRE_EQ(continued->cleanups.size(), 1);
    CHECK_EQ(continued->cleanups[0].bindingId, innerValue->bindingId);
    CHECK_EQ(continued->label, "inner");
}

TEST_CASE("move assignment schedules conditional destruction before replacing droppable storage") {
    const HirPackage package = LowerConsumptionHir(R"(
        struct Handle { value: int32; }
        extend Handle {
            func =(self: &var Handle, other: &Handle);
            func ~Handle(self: &var Handle) {}
        }
        struct Owner { handle: Handle; }

        func Replace(destination: Handle, source: Handle) {
            var target <- destination;
            target <- source;
        }

        func ReplaceField(owner: Owner, source: Handle) {
            var target <- owner;
            target.handle <- source;
        }

        func Initialize(source: Handle) {
            var destination: Handle;
            destination <- source;
        }
    )");

    const HirFunc &replace = RequireFunction(package, "Replace");
    REQUIRE(replace.body.has_value());
    REQUIRE_EQ(replace.params.size(), 2);
    const auto *target = dynamic_cast<const HirLetStmt *>(replace.body->stmts[0].get());
    const auto *statement = dynamic_cast<const HirExprStmt *>(replace.body->stmts[1].get());
    REQUIRE(target != nullptr);
    REQUIRE(statement != nullptr);
    const auto *assignment = dynamic_cast<const HirAssignExpr *>(statement->expr.get());
    REQUIRE(assignment != nullptr);
    REQUIRE(assignment->overwriteCleanup.has_value());
    CHECK_EQ(assignment->overwriteCleanup->bindingId, target->bindingId);
    CHECK_EQ(assignment->overwriteCleanup->glueSymbol, "__rux_drop__Handle");
    CHECK_EQ(assignment->value->consumedBindingId, replace.params[1].bindingId);

    const HirFunc &replaceField = RequireFunction(package, "ReplaceField");
    REQUIRE(replaceField.body.has_value());
    const auto *fieldStatement = dynamic_cast<const HirExprStmt *>(replaceField.body->stmts[1].get());
    REQUIRE(fieldStatement != nullptr);
    const auto *fieldAssignment = dynamic_cast<const HirAssignExpr *>(fieldStatement->expr.get());
    REQUIRE(fieldAssignment != nullptr);
    REQUIRE(fieldAssignment->overwriteCleanup.has_value());
    CHECK_EQ(fieldAssignment->overwriteCleanup->bindingId, 0);
    CHECK_EQ(fieldAssignment->overwriteCleanup->type, TypeRef::MakeNamed("Handle"));

    const HirFunc &initialize = RequireFunction(package, "Initialize");
    REQUIRE(initialize.body.has_value());
    const auto *uninitialized = dynamic_cast<const HirLetStmt *>(initialize.body->stmts[0].get());
    const auto *initializationStatement = dynamic_cast<const HirExprStmt *>(initialize.body->stmts[1].get());
    REQUIRE(uninitialized != nullptr);
    REQUIRE(initializationStatement != nullptr);
    CHECK(uninitialized->init == nullptr);
    const auto *initialization = dynamic_cast<const HirAssignExpr *>(initializationStatement->expr.get());
    REQUIRE(initialization != nullptr);
    REQUIRE(initialization->overwriteCleanup.has_value());
    CHECK_EQ(initialization->overwriteCleanup->bindingId, uninitialized->bindingId);
}

TEST_CASE("aggregate initialization records reverse rollback prefixes for completed droppable components") {
    const HirPackage package = LowerConsumptionHir(R"(
        struct Handle { value: int32; }
        extend Handle {
            func =(self: &var Handle, other: &Handle);
            func ~Handle(self: &var Handle) {}
        }
        struct Owner { first: Handle; copy: int32; last: Handle; }
        variant Choice { Both(Handle, Handle) }

        func Build(first: Handle, last: Handle) -> Owner {
            return Owner { first: <-first, copy: 1, last: <-last };
        }

        func Aggregate(a: Handle, b: Handle, c: Handle, d: Handle, e: Handle, f: Handle) {
            let array = [<-a, <-b];
            let tuple = (<-c, 1, <-d);
            let choice = Choice::Both(<-e, <-f);
        }
    )");

    const HirFunc &build = RequireFunction(package, "Build");
    REQUIRE(build.body.has_value());
    const auto *returned = dynamic_cast<const HirReturnStmt *>(build.body->stmts[0].get());
    REQUIRE(returned != nullptr);
    REQUIRE(returned->value.has_value());
    const auto *owner = dynamic_cast<const HirStructInitExpr *>((*returned->value).get());
    REQUIRE(owner != nullptr);
    REQUIRE_EQ(owner->failureCleanups.size(), 3);
    CHECK(owner->failureCleanups[0].empty());
    REQUIRE_EQ(owner->failureCleanups[1].size(), 1);
    CHECK_EQ(owner->failureCleanups[1][0].name, "first");
    REQUIRE_EQ(owner->failureCleanups[2].size(), 1);
    CHECK_EQ(owner->failureCleanups[2][0].kind, HirPartialDropAction::Kind::Field);

    const HirFunc &aggregate = RequireFunction(package, "Aggregate");
    REQUIRE(aggregate.body.has_value());
    const auto *arrayLet = dynamic_cast<const HirLetStmt *>(aggregate.body->stmts[0].get());
    const auto *tupleLet = dynamic_cast<const HirLetStmt *>(aggregate.body->stmts[1].get());
    const auto *choiceLet = dynamic_cast<const HirLetStmt *>(aggregate.body->stmts[2].get());
    REQUIRE(arrayLet != nullptr);
    REQUIRE(tupleLet != nullptr);
    REQUIRE(choiceLet != nullptr);
    const auto *array = dynamic_cast<const HirArrayExpr *>(arrayLet->init.get());
    const auto *tuple = dynamic_cast<const HirTupleExpr *>(tupleLet->init.get());
    const auto *choice = dynamic_cast<const HirEnumConstructExpr *>(choiceLet->init.get());
    REQUIRE(array != nullptr);
    REQUIRE(tuple != nullptr);
    REQUIRE(choice != nullptr);
    REQUIRE_EQ(array->failureCleanups[1].size(), 1);
    CHECK_EQ(array->failureCleanups[1][0].ordinal, 0);
    REQUIRE_EQ(tuple->failureCleanups[2].size(), 1);
    CHECK_EQ(tuple->failureCleanups[2][0].ordinal, 0);
    REQUIRE_EQ(choice->failureCleanups[1].size(), 1);
    CHECK_EQ(choice->failureCleanups[1][0].kind, HirPartialDropAction::Kind::EnumPayload);
}

TEST_CASE("partial and self moves are rejected before ownership state changes") {
    const std::vector<SemanticDiagnostic> diagnostics = AnalyzeConsumptionDiagnostics(R"(
        struct Handle { value: int32; }
        extend Handle {
            func =(self: &var Handle, other: &Handle);
            func ~Handle(self: &var Handle) {}
        }
        struct Owner { handle: Handle; }

        func Take(value: Handle) {}

        func Invalid(inputHandle: Handle, inputOwner: Owner, values: Handle[2], pointer: *Handle) {
            var handle <- inputHandle;
            var owner <- inputOwner;
            Take(<-owner.handle);
            Take(<-values[0]);
            Take(<-*pointer);
            handle <- handle;
            owner.handle <- owner.handle;
        }
    )");

    const std::vector<std::string> expected = {
        "cannot move field 'handle' out of droppable value 'owner'",
        "cannot move indexed element [0] out of droppable value 'values'",
        "cannot move '*pointer' out of borrowed pointer storage",
        "cannot move 'handle' into itself",
        "cannot move 'owner.handle' into itself",
    };
    REQUIRE_EQ(diagnostics.size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        CHECK_EQ(diagnostics[index].message, expected[index]);
    }
}

TEST_CASE("complete moved bindings can be reinitialized and consumed again") {
    const std::vector<SemanticDiagnostic> diagnostics = AnalyzeConsumptionDiagnostics(R"(
        struct Handle { value: int32; }
        extend Handle {
            func =(self: &var Handle, other: &Handle);
            func ~Handle(self: &var Handle) {}
        }

        func NewHandle(value: int32) -> Handle { return Handle { value: value }; }
        func Take(value: Handle) {}

        func Valid() {
            var reused = NewHandle(1);
            Take(<-reused);
            reused <- NewHandle(2);
            Take(<-reused);

            var initializedLater: Handle;
            initializedLater <- NewHandle(3);
            Take(<-initializedLater);
        }
    )");

    CHECK(diagnostics.empty());
}
