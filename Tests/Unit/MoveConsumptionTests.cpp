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

TEST_CASE("the move keyword is a unary prefix with ordinary precedence in every position") {
    Lexer lexer(R"(
        func Positions(a: int32, b: int32, pair: (int32, int32), holder: Holder, opt: int32?) -> int32 {
            let sum = move a + b;
            let field = move holder.size;
            let (left, right) = move pair;
            let propagated = (move opt)?;
            sum = move b;
            match move opt { _ => {} }
            if move opt is .Some(v) {}
            return move left;
        }
    )",
                "move_keyword.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "move_keyword.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    const auto *function = dynamic_cast<const FuncDecl *>(parsed.module.items[0].get());
    REQUIRE(function != nullptr);
    REQUIRE(function->body != nullptr);
    const auto &statements = function->body->stmts;
    REQUIRE_EQ(statements.size(), 8);
    const auto initializer = [&](const std::size_t index) -> const Expr * {
        const auto *binding = dynamic_cast<const LetStmt *>(statements[index].get());
        REQUIRE(binding != nullptr);
        return binding->init.get();
    };
    const auto movedOperand = [](const Expr *expression) -> const Expr * {
        const auto *move = dynamic_cast<const MoveExpr *>(expression);
        REQUIRE(move != nullptr);
        return move->operand.get();
    };

    // `move a + b` is `(move a) + b`.
    const auto *sum = dynamic_cast<const BinaryExpr *>(initializer(0));
    REQUIRE(sum != nullptr);
    CHECK(dynamic_cast<const IdentExpr *>(movedOperand(sum->left.get())) != nullptr);
    CHECK(dynamic_cast<const IdentExpr *>(sum->right.get()) != nullptr);
    // `move holder.size` moves the field.
    const auto *field = dynamic_cast<const FieldExpr *>(movedOperand(initializer(1)));
    REQUIRE(field != nullptr);
    CHECK_EQ(field->field, "size");
    // A destructuring binding takes the moved tuple.
    CHECK(dynamic_cast<const IdentExpr *>(movedOperand(initializer(2))) != nullptr);
    // `(move opt)?` moves before propagating.
    const auto *propagated = dynamic_cast<const TryExpr *>(initializer(3));
    REQUIRE(propagated != nullptr);
    CHECK(dynamic_cast<const MoveExpr *>(propagated->operand.get()) != nullptr);
    // `a = move b` is an ordinary assignment whose value is moved.
    const auto *statement = dynamic_cast<const ExprStmt *>(statements[4].get());
    REQUIRE(statement != nullptr);
    const auto *assignment = dynamic_cast<const AssignExpr *>(statement->expr.get());
    REQUIRE(assignment != nullptr);
    CHECK_EQ(assignment->op, TokenKind::Assign);
    CHECK(dynamic_cast<const MoveExpr *>(assignment->value.get()) != nullptr);
    const auto *match = dynamic_cast<const MatchStmt *>(statements[5].get());
    REQUIRE(match != nullptr);
    CHECK(dynamic_cast<const MoveExpr *>(match->subject.get()) != nullptr);
    // `move opt is .Some(v)` tests the moved subject.
    const auto *test = dynamic_cast<const IfStmt *>(statements[6].get());
    REQUIRE(test != nullptr);
    const auto *is = dynamic_cast<const IsExpr *>(test->condition.get());
    REQUIRE(is != nullptr);
    CHECK(dynamic_cast<const MoveExpr *>(is->operand.get()) != nullptr);
    const auto *returned = dynamic_cast<const ReturnStmt *>(statements[7].get());
    REQUIRE(returned != nullptr);
    REQUIRE(returned->value.has_value());
    CHECK(dynamic_cast<const MoveExpr *>(returned->value->get()) != nullptr);
}

TEST_CASE("move is a reserved keyword that cannot name a binding") {
    Lexer lexer("func Bind() { let move = 1; }", "move_binding.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "move_binding.rux");
    const auto parsed = parser.Parse();
    REQUIRE_EQ(parsed.diagnostics.size(), 1);
    CHECK_EQ(parsed.diagnostics[0].message, "'move' is a reserved keyword and cannot name a binding");
    CHECK_EQ(parsed.diagnostics[0].help, "choose another name, such as 'moved'");
}

TEST_CASE("move and the older arrow record the same consumption facts") {
    const std::string source = R"(
        struct Handle { value: int32; }
        extend Handle {
            func =(self: &var Handle, other: &Handle);
            func ~Handle(self: &var Handle) {}
        }
        func Take(handle: Handle) {}
        func Keyword(source: Handle, other: Handle) {
            let bound = move source;
            var target = move other;
            target = move bound;
            Take(move target);
        }
        func Arrow(source: Handle, other: Handle) {
            let bound <- source;
            var target <- other;
            target <- bound;
            Take(<-target);
        }
    )";
    Lexer lexer(source, "move_facts.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "move_facts.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());

    // Each function moves four named places. Whatever the spelling, each one is consumed as an explicit move of a
    // 'Handle', and nothing is copied.
    for (const std::size_t item : {3U, 4U}) {
        CAPTURE(item);
        const auto *function = dynamic_cast<const FuncDecl *>(parsed.module.items[item].get());
        REQUIRE(function != nullptr);
        std::vector<const Expr *> sources;
        for (const auto &statement : function->body->stmts) {
            const Expr *value = nullptr;
            if (const auto *binding = dynamic_cast<const LetStmt *>(statement.get())) {
                value = binding->init.get();
            }
            else if (const auto *expression = dynamic_cast<const ExprStmt *>(statement.get())) {
                if (const auto *assignment = dynamic_cast<const AssignExpr *>(expression->expr.get())) {
                    value = assignment->value.get();
                }
                else if (const auto *call = dynamic_cast<const CallExpr *>(expression->expr.get())) {
                    value = call->args[0].get();
                }
            }
            REQUIRE(value != nullptr);
            if (const auto *move = dynamic_cast<const MoveExpr *>(value)) {
                value = move->operand.get();
            }
            sources.push_back(value);
        }
        REQUIRE_EQ(sources.size(), 4);
        for (const Expr *moved : sources) {
            const ValueConsumption *fact = model.TryGetConsumption(*moved);
            REQUIRE(fact != nullptr);
            CHECK_EQ(fact->kind, ValueConsumptionKind::ExplicitMove);
            CHECK_EQ(fact->type, TypeRef::MakeNamed("Handle"));
        }
    }
}

TEST_CASE("move keeps every ownership guarantee of the older arrow") {
    const std::vector<SemanticDiagnostic> diagnostics = AnalyzeConsumptionDiagnostics(R"(
        struct Handle { value: int32; }
        extend Handle {
            func =(self: &var Handle, other: &Handle);
            func ~Handle(self: &var Handle) {}
        }
        func Take(handle: Handle) {}
        func UseAfter(source: Handle) -> int32 {
            Take(move source);
            return source.value;
        }
        func Copyable(count: int32) -> int32 {
            let moved = move count;
            return count;
        }
        func SelfMove(source: Handle) {
            var target = move source;
            target = move target;
        }
        func FromReference(handle: &Handle) {
            let owned = move handle;
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 4);
    CHECK_EQ(diagnostics[0].message, "value 'source' is used after it was moved");
    CHECK_EQ(diagnostics[1].message, "value 'count' is used after it was moved");
    CHECK_EQ(diagnostics[2].message, "cannot move 'target' into itself");
    CHECK_EQ(diagnostics[3].message, "cannot move a non-owning reference");
    CHECK_EQ(diagnostics[3].help, "pass or assign the reference without 'move', or move the owning value instead");
}

TEST_CASE("explicit moves invalidate copyable places and reject invalid ownership sources") {
    const std::vector<SemanticDiagnostic> diagnostics = AnalyzeConsumptionDiagnostics(R"(
        struct Pair { first: int32; second: int32; }
        struct Handle { value: int32; }
        extend Handle {
            func =(self: &var Handle, other: &Handle);
            func ~Handle(self: &var Handle) {}
        }

        // Safety: `pointer` must address a live `Pair`
        unsafe func ThroughPointer(pointer: *Pair) {
            let pointee <- *pointer;
        }

        func ThroughReference(pair: &Pair) {
            let field <- pair.first;
        }

        func MoveReference(pair: &Pair) {
            let alias <- pair;
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
    for (const SemanticDiagnostic &diagnostic : diagnostics) {
        CHECK_EQ(diagnostic.message, "cannot copy 'Handle', which owns resources");
        CHECK_EQ(diagnostic.help, "write 'move source' to transfer it, or 'source.Clone()' to copy it");
    }
    REQUIRE_EQ(diagnostics[0].notes.size(), 1);
    CHECK_EQ(diagnostics[0].notes[0],
             "a named value used by value in initialization position is copied, and only plain data copies");
    REQUIRE_EQ(diagnostics[1].notes.size(), 1);
    CHECK_EQ(diagnostics[1].notes[0],
             "a named value used by value in argument position is copied, and only plain data copies");
    REQUIRE_EQ(diagnostics[2].notes.size(), 1);
    CHECK_EQ(diagnostics[2].notes[0],
             "a named value used by value in return position is copied, and only plain data copies");
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

        // Safety: `slot` must address a live, writable `T`
        unsafe func Store<T>(slot: *var T, value: T) {
            *slot <- value;
        }

        func Main() {
            var room = Handle { value: 0 };
            // Safety: `@room` addresses a live local `Handle`
            unsafe { Store<Handle>(@room, Handle { value: 1 }); }
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

        // Safety: `held` must address a live `Held`
        unsafe func Borrowed(held: *Held) {
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

TEST_CASE("a by-value match owns the copy it makes of a named plain-data subject") {
    // The copy is a value nobody else holds, so the arms own it as they own a subject handed over with `move`. A
    // subject that stays with its owner, because nothing is bound from it or it is read through a borrow, is not
    // owned. A copy is the subject's bytes, so no fact beyond the ownership is recorded for it.
    Lexer lexer(R"(
        struct Counted { id: int32; }
        variant Held { Full(Counted), Empty }

        func Make() -> Held { return Held::Empty; }

        func Copied(held: Held) {
            match held {
                .Full(item) => {},
                .Empty => {}
            }
        }

        func LookOnly(held: Held) {
            match held {
                .Full(_) => {},
                .Empty => {}
            }
        }

        func Temporary() {
            match Make() {
                .Full(item) => {},
                .Empty => {}
            }
        }

        func Moved(held: Held) {
            match <-held {
                .Full(_) => {},
                .Empty => {}
            }
        }

        // Safety: `held` must address a live `Held`
        unsafe func Borrowed(held: *Held) {
            match *held {
                .Full(item) => {},
                .Empty => {}
            }
        }

        func Referenced(held: &Held) {
            match held {
                .Full(item) => {},
                .Empty => {}
            }
        }
    )",
                "match_copy.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "match_copy.rux");
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

    CHECK(model.IsOwnedMatchSubject(subjectOf(3)));
    CHECK_FALSE(model.IsOwnedMatchSubject(subjectOf(4)));
    CHECK(model.IsOwnedMatchSubject(subjectOf(5)));
    CHECK(model.IsOwnedMatchSubject(subjectOf(6)));
    CHECK_FALSE(model.IsOwnedMatchSubject(subjectOf(7)));
    CHECK_FALSE(model.IsOwnedMatchSubject(subjectOf(8)));
}

TEST_CASE("assignment transfers a fresh temporary and never copies a named owning value") {
    // A fresh temporary and a value handed over with `move` have no other owner, so they transfer into the target as
    // they stand, whether the target is a local or a referent written through `&var`. A named place or borrowed
    // storage keeps its value, and an owning type cannot be copied out of it: the copy is an explicit `Clone`.
    const std::vector<SemanticDiagnostic> diagnostics = AnalyzeConsumptionDiagnostics(R"(
        struct Counted { id: int32; }
        extend Counted {
            func Counted(id: int32) -> Counted { return Counted { id: id }; }
            func Clone(&self) -> Counted { return Counted { id: self.id }; }
            func ~Counted(self: &var Counted) {}
        }
        struct Holder { item: Counted; }
        func Make(id: int32) -> Counted { return Counted { id: id }; }

        func Assign(flag: bool, which: int32, named: Counted, holder: Holder, pointer: *Counted, slot: &var Counted) {
            var item = Make(0);
            var moved = Make(9);
            item = Make(1);
            item = Counted { id: 2 };
            item = Counted(3);
            item = flag ? Make(4) : Make(5);
            item = match which { 6 => Make(6), else => Make(7) };
            item = move moved;
            slot = Make(8);
            item = named.Clone();
            item = holder.item.Clone();
            // Safety: callers pass `pointer` addressing a live `Counted`
            unsafe { item = (*pointer).Clone(); }
            slot = named.Clone();
            item = named;
            item = holder.item;
            slot = named;
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 3);
    for (const SemanticDiagnostic &diagnostic : diagnostics) {
        CHECK_EQ(diagnostic.message, "cannot copy 'Counted', which owns resources");
    }
    CHECK_EQ(diagnostics[0].help, "write 'move named' to transfer it, or 'named.Clone()' to copy it");
    CHECK_EQ(diagnostics[1].help, "write 'move holder.item' to transfer it, or 'holder.item.Clone()' to copy it");
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

        // Safety: `pointer` must address a live `Handle`
        unsafe func Invalid(inputHandle: Handle, inputOwner: Owner, values: Handle[2], pointer: *Handle) {
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
