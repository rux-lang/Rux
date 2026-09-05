#include "MoveConsumptionTestSupport.h"

using namespace Rux;
using namespace Rux::Testing::MoveConsumptionTestSupport;

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
