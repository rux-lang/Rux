#include "MoveConsumptionTestSupport.h"

using namespace Rux;
using namespace Rux::Testing::MoveConsumptionTestSupport;

TEST_CASE("named moves read their source as it stands and run no code") {
    // A move is the value's bytes followed by forgetting the source, so nothing wraps the read: no copy and no hook.
    // The consumption fact is what keeps the source's destructor from running as well.
    const std::string source = R"(

        struct Cell { value: int32; }
        extend Cell {
            func =(self: &var Cell, other: &Cell);
            func ~Cell(self: &var Cell) {}
        }

        struct Wrapper { cell: Cell; tag: int32; }

        func Move(source: Wrapper) -> Wrapper {
            let local = move source;
            return move local;
        }
        func Older(source: Wrapper) -> Wrapper {
            let local <- source;
            return <-local;
        }
    )";

    const HirPackage hir = LowerConsumptionHir(source);
    for (const std::string name : {"Move", "Older"}) {
        CAPTURE(name);
        const HirFunc &move = RequireFunction(hir, name);
        REQUIRE(move.body.has_value());
        const auto *local = dynamic_cast<const HirLetStmt *>(move.body->stmts[0].get());
        const auto *returned = dynamic_cast<const HirReturnStmt *>(move.body->stmts[1].get());
        REQUIRE(local != nullptr);
        REQUIRE(returned != nullptr);
        REQUIRE(returned->value.has_value());
        const auto *localRead = dynamic_cast<const HirVarExpr *>(local->init.get());
        const auto *returnRead = dynamic_cast<const HirVarExpr *>((*returned->value).get());
        REQUIRE(localRead != nullptr);
        REQUIRE(returnRead != nullptr);
        CHECK_EQ(localRead->consumption, ValueConsumptionKind::ExplicitMove);
        CHECK_EQ(localRead->consumedBindingId, move.params[0].bindingId);
        CHECK_EQ(returnRead->consumedBindingId, local->bindingId);
    }
}

TEST_CASE("move assignment destroys the replaced destination through its conditional cleanup") {
    const std::string source = R"(
        struct Cell { value: int32; }
        extend Cell {
            func =(self: &var Cell, other: &Cell);
            func ~Cell(self: &var Cell) {}
        }

        func Replace(destination: Cell, source: Cell) {
            var target = move destination;
            target = move source;
        }
        func Initialize(source: Cell) {
            var destination: Cell;
            destination = move source;
        }
        func Older(destination: Cell, source: Cell) {
            var target <- destination;
            target <- source;
        }
    )";

    const HirPackage hir = LowerConsumptionHir(source);
    for (const std::string name : {"Replace", "Initialize", "Older"}) {
        CAPTURE(name);
        const HirFunc &function = RequireFunction(hir, name);
        REQUIRE(function.body.has_value());
        const auto *statement = dynamic_cast<const HirExprStmt *>(function.body->stmts[1].get());
        REQUIRE(statement != nullptr);
        const auto *assignment = dynamic_cast<const HirAssignExpr *>(statement->expr.get());
        REQUIRE(assignment != nullptr);
        REQUIRE(assignment->overwriteCleanup.has_value());
        CHECK_NE(assignment->overwriteCleanup->bindingId, 0);
        const auto *read = dynamic_cast<const HirVarExpr *>(assignment->value.get());
        REQUIRE(read != nullptr);
        CHECK_EQ(read->consumption, ValueConsumptionKind::ExplicitMove);
        CHECK_NE(read->consumedBindingId, 0);
    }
}

TEST_CASE("custom move hooks are rejected and the type keeps its generated move") {
    const std::vector<SemanticDiagnostic> diagnostics = AnalyzeConsumptionDiagnostics(R"(
        struct Cursor { position: int32; }
        extend Cursor {
            func <-(&var self, other: Cursor) { self.position = other.position; }
        }
        func Transfer(source: Cursor) -> Cursor {
            let moved = move source;
            return move moved;
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics[0].message, "custom move hooks are not supported");
    CHECK_EQ(diagnostics[0].help, "a type that must stay in place uses '#NoMove'");
}

TEST_CASE("'#NoMove' values are built in place and never moved") {
    const std::vector<SemanticDiagnostic> diagnostics = AnalyzeConsumptionDiagnostics(R"(
        #NoMove
        struct Node { next: *var Node; }
        extend Node {
            func ~Node(&var self) {}
        }
        func Build() -> Node { return Node { next: null }; }
        func Accept(node: Node) {}
        func InPlace() -> int32 {
            let built = Build();
            let literal = Node { next: null };
            Accept(Build());
            return 0;
        }
        func Moves(node: Node) {
            let moved = move node;
        }
        func Copies(node: Node) {
            let copied = node;
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 2);
    CHECK_EQ(diagnostics[0].message, "moving type 'Node' is prohibited");
    REQUIRE_EQ(diagnostics[0].notes.size(), 1);
    CHECK_EQ(diagnostics[0].notes[0], "a '#NoMove' type, or one holding a '#NoMove' value, stays where it was built");
    CHECK_EQ(diagnostics[0].help, "borrow the value or construct a distinct replacement instead");
    CHECK_EQ(diagnostics[1].message, "move-only value 'node' requires an explicit 'move' in initialization");
}

TEST_CASE("a bodyless '=' and '#NoCopy' make the same move-only type") {
    const std::string body = R"(
        func Make() -> Handle { return Handle { value: 1 }; }
        func Uses(source: Handle) -> int32 {
            var target = Make();
            target = Make();
            target = move source;
            let copied = target;
            return 0;
        }
    )";
    const std::vector<SemanticDiagnostic> attribute = AnalyzeConsumptionDiagnostics(R"(
        #NoCopy
        struct Handle { value: int32; }
    )" + body);
    const std::vector<SemanticDiagnostic> operation = AnalyzeConsumptionDiagnostics(R"(
        struct Handle { value: int32; }
        extend Handle { func =(&var self, other: &Handle); }
    )" + body);

    for (const auto *diagnostics : {&attribute, &operation}) {
        REQUIRE_EQ(diagnostics->size(), 1);
        CHECK_EQ(diagnostics->front().message,
                 "move-only value 'target' requires an explicit 'move' in initialization");
        CHECK_EQ(diagnostics->front().help, "write 'let destination = move target' to transfer ownership");
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

TEST_CASE("copy assignment installs a fresh temporary without calling the custom copy") {
    const std::string source = R"(

        struct Cell { value: int32; }
        extend Cell {
            func ~Cell(self: &var Cell) {}
            func =(self: &var Cell, other: &Cell) {
                self.value = other.value;
            }
        }

        func Make(value: int32) -> Cell { return Cell { value: value }; }

        func Replace(named: Cell, slot: &var Cell) {
            var item = Make(0);
            item = Make(1);
            item = named;
            slot = Cell { value: 2 };
            slot = named;
        }
    )";

    const HirPackage hir = LowerConsumptionHir(source);
    const HirFunc &replace = RequireFunction(hir, "Replace");
    REQUIRE(replace.body.has_value());
    std::vector<const HirAssignExpr *> assignments;
    for (const HirStmtPtr &statement : replace.body->stmts) {
        if (const auto *expression = dynamic_cast<const HirExprStmt *>(statement.get())) {
            if (const auto *assignment = dynamic_cast<const HirAssignExpr *>(expression->expr.get())) {
                REQUIRE(assignment->overwriteCleanup.has_value());
                assignments.push_back(assignment);
            }
        }
    }
    REQUIRE_EQ(assignments.size(), 4);

    CHECK(dynamic_cast<const HirCopyExpr *>(assignments[0]->value.get()) == nullptr);
    CHECK(dynamic_cast<const HirCopyExpr *>(assignments[2]->value.get()) == nullptr);
    CHECK(assignments[2]->overwriteCleanup->destroysTarget);
    const auto *namedCopy = dynamic_cast<const HirCopyExpr *>(assignments[1]->value.get());
    REQUIRE(namedCopy != nullptr);
    REQUIRE(dynamic_cast<const HirCopyExpr *>(assignments[3]->value.get()) != nullptr);
    CHECK_EQ(namedCopy->plan.kind, HirCopyPlan::Kind::Custom);
    const std::string copySymbol = namedCopy->plan.customCallee;
    CHECK_FALSE(copySymbol.empty());

    const LirPackage lir = LowerConsumptionLir(source);
    CHECK_EQ(LirCallCount(RequireLirFunction(lir, "Replace"), copySymbol), 2);
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

TEST_CASE("variant moves are byte copies with no per-case dispatch") {
    const std::string source = R"(
        struct Handle { value: int32; }
        extend Handle {
            func =(self: &var Handle, other: &Handle);
            func ~Handle(self: &var Handle) {}
        }

        variant Choice {
            Empty,
            Direct(Handle),
            Pair(int32, Handle),
            Named { sequence: uint64; handle: Handle; }
        }
        variant Maybe<T> { None, Some(T) }
        variant Envelope<T> { Empty, Direct(T), Nested(Maybe<T>) }

        func Take(value: Choice) {}
        func Transfer(source: Choice) {
            let moved = move source;
            Take(move moved);
        }
        func Nested(source: Envelope<Handle>) {
            let moved = move source;
        }
    )";

    const HirPackage hir = LowerConsumptionHir(source);
    for (const std::string name : {"Transfer", "Nested"}) {
        CAPTURE(name);
        const HirFunc &transfer = RequireFunction(hir, name);
        REQUIRE(transfer.body.has_value());
        const auto *binding = dynamic_cast<const HirLetStmt *>(transfer.body->stmts.front().get());
        REQUIRE(binding != nullptr);
        const auto *read = dynamic_cast<const HirVarExpr *>(binding->init.get());
        REQUIRE(read != nullptr);
        CHECK_EQ(read->consumption, ValueConsumptionKind::ExplicitMove);
    }
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
