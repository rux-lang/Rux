#include "MoveConsumptionTestSupport.h"

using namespace Rux;
using namespace Rux::Testing::MoveConsumptionTestSupport;

TEST_CASE("named moves read their source as it stands and run no code") {
    // A move is the value's bytes followed by forgetting the source, so nothing wraps the read: no copy and no hook.
    // The consumption fact is what keeps the source's destructor from running as well.
    const std::string source = R"(

        struct Cell { value: int32; }
        extend Cell {
            func ~Cell(&var self) {}
        }

        struct Wrapper { cell: Cell; tag: int32; }

        func Move(source: Wrapper) -> Wrapper {
            let local = move source;
            return move local;
        }
        func Older(source: Wrapper) -> Wrapper {
            let local = move source;
            return move local;
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
            func ~Cell(&var self) {}
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
            var target = move destination;
            target = move source;
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
    CHECK_EQ(diagnostics[1].message, "cannot copy 'Node', which owns resources");
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
        #NoCopy
        struct Handle { value: int32; }
        extend Handle { }
    )" + body);

    for (const auto *diagnostics : {&attribute, &operation}) {
        REQUIRE_EQ(diagnostics->size(), 1);
        CHECK_EQ(diagnostics->front().message, "cannot copy 'Handle', which prohibits copying");
        CHECK_EQ(diagnostics->front().help, "write 'move target' to transfer it");
    }
}

TEST_CASE("prohibited move operations reject explicit ownership transfer") {
    const std::vector<SemanticDiagnostic> diagnostics = AnalyzeConsumptionDiagnostics(R"(
        #NoCopy
        #NoMove
        struct Pinned { value: int32; }
        extend Pinned {
        }

        func Transfer(source: Pinned) {
            let destination = move source;
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics[0].message, "moving type 'Pinned' is prohibited");
}

TEST_CASE("a copy of plain data reads its source as it stands and runs no code") {
    // A copyable type has no destructor anywhere inside it, so a copy is the value's bytes: nothing wraps the read,
    // nothing is consumed, and no call is made for it.
    const std::string source = R"(

        struct Cell { value: int32; }
        struct Wrapper { cell: Cell; tag: int32; }
        variant Maybe<T> { Some(T), None }

        func Take(value: Wrapper) {}
        func TakeMaybe(value: Maybe<Cell>) {}
        func Copy(source: Wrapper, maybe: Maybe<Cell>) -> Wrapper {
            let local = source;
            Take(source);
            TakeMaybe(maybe);
            return source;
        }
    )";

    const HirPackage hir = LowerConsumptionHir(source);
    const HirFunc &copy = RequireFunction(hir, "Copy");
    REQUIRE(copy.body.has_value());
    const auto *local = dynamic_cast<const HirLetStmt *>(copy.body->stmts[0].get());
    const auto *returned = dynamic_cast<const HirReturnStmt *>(copy.body->stmts[3].get());
    REQUIRE(local != nullptr);
    REQUIRE(returned != nullptr);
    REQUIRE(returned->value.has_value());
    const auto *localRead = dynamic_cast<const HirVarExpr *>(local->init.get());
    const auto *returnRead = dynamic_cast<const HirVarExpr *>((*returned->value).get());
    REQUIRE(localRead != nullptr);
    REQUIRE(returnRead != nullptr);
    CHECK_FALSE(localRead->consumption.has_value());
    CHECK_FALSE(returnRead->consumption.has_value());

    const LirPackage lir = LowerConsumptionLir(source);
    const LirFunc &lowered = RequireLirFunction(lir, "Copy");
    std::size_t calls = 0;
    for (const LirBlock &block : lowered.blocks) {
        calls += static_cast<std::size_t>(std::ranges::count_if(
            block.instrs, [](const LirInstr &instruction) { return instruction.op == LirOpcode::Call; }));
    }
    CHECK_EQ(calls, 2);
    CHECK_FALSE(std::ranges::any_of(lowered.blocks,
                                    [](const LirBlock &block) { return block.label.starts_with("copy.variant"); }));
}

TEST_CASE("a custom copy hook is rejected and copying stays a byte copy or a move-only error") {
    const std::vector<SemanticDiagnostic> diagnostics = AnalyzeConsumptionDiagnostics(R"(
        struct Plain { value: int32; }
        extend Plain {
            func =(&var self, other: &Plain) { self.value = other.value + 1; }
        }
        struct Seed { value: int32; }
        struct Owning { value: int32; }
        extend Owning {
            func =(&var self, other: &Seed) { self.value = other.value; }
            func ~Owning(&var self) {}
        }
        func Copies(plain: Plain, owning: Owning) -> int32 {
            let copied = plain;
            let other = owning;
            return copied.value;
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 3);
    CHECK_EQ(diagnostics[0].message, "custom copy hooks are not supported");
    CHECK_EQ(diagnostics[0].help,
             "implement 'func Clone(&self) -> Self' on 'Plain' and call it where a copy is needed");
    CHECK_EQ(diagnostics[1].message, "custom copy hooks are not supported");
    CHECK_EQ(diagnostics[2].message, "cannot copy 'Owning', which owns resources");
    CHECK_EQ(diagnostics[2].help, "write 'move owning' to transfer it");
}

TEST_CASE("an explicit Clone replaces a destination with the value it returns") {
    const std::string source = R"(

        struct Cell { value: int32; }
        extend Cell {
            func Clone(&self) -> Cell { return Cell { value: self.value }; }
            func ~Cell(&var self) {}
        }

        func Replace(named: Cell, slot: &var Cell) {
            var item = named.Clone();
            item = named.Clone();
            slot = named.Clone();
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
    REQUIRE_EQ(assignments.size(), 2);
    for (const HirAssignExpr *assignment : assignments) {
        CHECK(dynamic_cast<const HirCallExpr *>(assignment->value.get()) != nullptr);
    }
    CHECK(assignments[1]->overwriteCleanup->destroysTarget);
}

TEST_CASE("variant moves are byte copies with no per-case dispatch") {
    const std::string source = R"(
        struct Handle { value: int32; }
        extend Handle {
            func ~Handle(&var self) {}
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
    CHECK(dynamic_cast<const HirVarExpr *>(binding->init.get()) != nullptr);
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
            func ~Handle(&var self) {}
        }
        variant Held { Empty, Full(Handle) }

        func Consume(value: Held) {
            match move value { .Empty => {}, .Full(handle) => {} }
            value;
        }

        // Safety: `value` must address a live `Held`
        unsafe func Inspect(value: *Held) {
            match *value { .Empty => {}, .Full(handle) => {} }
            match *value { .Empty => {}, .Full(_) => {} }
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics.front().message, "value 'value' is used after it was moved");
}
