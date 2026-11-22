#include "MoveConsumptionTestSupport.h"

using namespace Rux;
using namespace Rux::Testing::MoveConsumptionTestSupport;

TEST_CASE("a bare value is given away in bindings assignments calls and returns") {
    Lexer lexer(R"(
        func Take(value: int32) -> int32 { return value; }
        func Transfer(source: int32) -> int32 {
            let bound = source;
            var destination = 0;
            destination = bound;
            return Take(destination);
        }
    )",
                "bare_transfer_syntax.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "bare_transfer_syntax.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    const auto *transfer = dynamic_cast<const FuncDecl *>(parsed.module.items[1].get());
    REQUIRE(transfer != nullptr);
    REQUIRE(transfer->body != nullptr);
    REQUIRE_EQ(transfer->body->stmts.size(), 4);
    const auto *bound = dynamic_cast<const LetStmt *>(transfer->body->stmts[0].get());
    REQUIRE(bound != nullptr);
    CHECK(dynamic_cast<const IdentExpr *>(bound->init.get()) != nullptr);
    const auto *assignmentStatement = dynamic_cast<const ExprStmt *>(transfer->body->stmts[2].get());
    REQUIRE(assignmentStatement != nullptr);
    const auto *assignment = dynamic_cast<const AssignExpr *>(assignmentStatement->expr.get());
    REQUIRE(assignment != nullptr);
    CHECK_EQ(assignment->op, TokenKind::Assign);
    CHECK(dynamic_cast<const IdentExpr *>(assignment->value.get()) != nullptr);
}

TEST_CASE("'move' and '<-' are rejected and the value after them is read as written") {
    Lexer lexer(R"(
        func Positions(a: int32, b: int32) -> int32 {
            let sum = move a + b;
            let bound <- b;
            var target = 0;
            target <- a;
            return <-target;
        }
    )",
                "move_removed.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "move_removed.rux");
    auto parsed = parser.Parse();
    REQUIRE_EQ(parsed.diagnostics.size(), 4);
    CHECK_EQ(parsed.diagnostics[0].message, "'move' is not supported");
    CHECK_EQ(parsed.diagnostics[0].help, "remove it: a bare value is given away");
    CHECK_EQ(parsed.diagnostics[1].message, "'<-' is not a binding operator");
    CHECK_EQ(parsed.diagnostics[1].help, "write 'let name = source'");
    CHECK_EQ(parsed.diagnostics[2].message, "'<-' is not an assignment operator");
    CHECK_EQ(parsed.diagnostics[2].help, "write 'target = source'");
    CHECK_EQ(parsed.diagnostics[3].message, "'<-' is not an operator");

    const auto *function = dynamic_cast<const FuncDecl *>(parsed.module.items[0].get());
    REQUIRE(function != nullptr);
    REQUIRE(function->body != nullptr);
    // `move a + b` reads as `a + b`.
    const auto *sum = dynamic_cast<const LetStmt *>(function->body->stmts[0].get());
    REQUIRE(sum != nullptr);
    const auto *addition = dynamic_cast<const BinaryExpr *>(sum->init.get());
    REQUIRE(addition != nullptr);
    CHECK(dynamic_cast<const IdentExpr *>(addition->left.get()) != nullptr);
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

TEST_CASE("a bare owning value records a transfer in every by-value position") {
    const std::string source = R"(
        struct Handle { value: int32; }
        extend Handle {
            destructor {}
        }
        func Take(handle: Handle) {}
        func Keyword(source: Handle, other: Handle) {
            let bound = source;
            var target = other;
            target = bound;
            Take(target);
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

    // The function gives away four named places, and each one is consumed as a transfer of a 'Handle'; nothing is
    // copied.
    for (const std::size_t item : {3U}) {
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
            sources.push_back(value);
        }
        REQUIRE_EQ(sources.size(), 4);
        for (const Expr *moved : sources) {
            const ValueConsumption *fact = model.TryGetConsumption(*moved);
            REQUIRE(fact != nullptr);
            CHECK_EQ(fact->type, TypeRef::MakeNamed("Handle"));
        }
    }
}

TEST_CASE("a bare transfer keeps every ownership guarantee") {
    const std::vector<SemanticDiagnostic> diagnostics = AnalyzeConsumptionDiagnostics(R"(
        struct Handle { value: int32; }
        extend Handle {
            destructor {}
        }
        func Take(handle: Handle) {}
        func UseAfter(source: Handle) -> int32 {
            Take(source);
            return source.value;
        }
        func Copies(count: int32) -> int32 {
            let copied = count;
            return count;
        }
        func SelfMove(source: Handle) {
            var target = source;
            target = target;
        }
        func FromReference(handle: &Handle) {
            let alias = handle;
        }
    )");

    // Plain data copies and a reference is passed on, so only the owning value is given away.
    REQUIRE_EQ(diagnostics.size(), 2);
    CHECK_EQ(diagnostics[0].message, "'source' is used after it was given away");
    CHECK_EQ(diagnostics[1].message, "cannot give 'target' to itself");
}

TEST_CASE("only a local, a parameter, a temporary or a read through a raw pointer is given away") {
    const std::vector<SemanticDiagnostic> diagnostics = AnalyzeConsumptionDiagnostics(R"(
        struct Handle { value: int32; }
        extend Handle {
            destructor {}
        }
        struct Holder { handle: Handle; count: int32; }

        // Safety: `pointer` must address two live `Handle`s, which the reads take out
        func ThroughPointer(pointer: *var Handle) {
            let pointee = *pointer;
            let element = pointer[1];
        }

        func ThroughReference(holder: &Holder) {
            let field = holder.handle;
        }

        func MoveBorrowedHandle() {
            let handle = Handle { value: 1 };
            let alias: &Handle = &handle;
            let moved = handle;
            alias.value;
        }

        func Partial() {
            let holder = Holder { handle: Handle { value: 3 }, count: 4 };
            let partial = holder.handle;
        }
    )");

    const std::vector<std::string> expected = {
        "'holder.handle' is reached through a loan and cannot be given away",
        "cannot move 'handle' while it is immutably borrowed",
        "'handle' is a field of 'holder' and cannot be given away by itself",
    };
    REQUIRE_EQ(diagnostics.size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        CHECK_EQ(diagnostics[index].message, expected[index]);
    }
    // A place that cannot be written cannot be taken with 'Replace' or 'Swap', so the help offers neither.
    CHECK_EQ(diagnostics[0].help, "clone it; it cannot be taken with 'Replace' or 'Swap', which write the place");
    CHECK_EQ(diagnostics[2].help,
             "take 'holder' apart with a pattern; it cannot be taken with 'Replace' or 'Swap', which write the place");
}

TEST_CASE("a part that can be written is offered 'Replace' and 'Swap'") {
    const std::vector<SemanticDiagnostic> diagnostics = AnalyzeConsumptionDiagnostics(R"(
        struct Handle { value: int32; }
        extend Handle {
            destructor {}
        }
        struct Holder { handle: Handle; count: int32; }

        func ThroughWritableReference(holder: &var Holder) {
            let field = holder.handle;
        }

        func Partial() {
            var holder = Holder { handle: Handle { value: 3 }, count: 4 };
            let partial = holder.handle;
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 2);
    CHECK_EQ(diagnostics[0].help, "take it with 'Replace(&var holder.handle, value)' or 'Swap', or clone it");
    CHECK_EQ(diagnostics[1].help,
             "take it with 'Replace(&var holder.handle, value)' or 'Swap', or take 'holder' apart with a pattern");
}

TEST_CASE("bare transfers retain source drop-flag transfers in HIR") {
    const HirPackage package = LowerConsumptionHir(R"(
        struct Handle { value: int32; }
        extend Handle {
            destructor {}
        }

        func Transfer(source: Handle) -> Handle {
            let local = source;
            return local;
        }

        func Replace(destination: Handle, source: Handle) {
            var target = destination;
            target = source;
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
    CHECK_EQ(local->init->consumption, ValueConsumptionKind::Initialization);
    CHECK_EQ(local->init->consumedBindingId, transfer.params[0].bindingId);
    CHECK_EQ((*returned->value)->consumption, ValueConsumptionKind::Return);
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
    CHECK_EQ(assignment->op, TokenKind::Assign);
    REQUIRE(assignment->overwriteCleanup.has_value());
    CHECK_EQ(assignment->overwriteCleanup->bindingId, target->bindingId);
    CHECK_EQ(assignment->value->consumption, ValueConsumptionKind::Assignment);
    CHECK_EQ(assignment->value->consumedBindingId, replace.params[1].bindingId);

    const HirFunc &fresh = RequireFunction(package, "Fresh");
    REQUIRE(fresh.body.has_value());
    const auto *freshReturn = dynamic_cast<const HirReturnStmt *>(fresh.body->stmts[0].get());
    REQUIRE(freshReturn != nullptr);
    REQUIRE(freshReturn->value.has_value());
    CHECK_EQ((*freshReturn->value)->consumption, ValueConsumptionKind::Return);
    CHECK_EQ((*freshReturn->value)->consumedBindingId, 0);
}

TEST_CASE("a named owning value written bare is given away in every by-value context") {
    const std::vector<SemanticDiagnostic> diagnostics = AnalyzeConsumptionDiagnostics(R"(

        struct Handle { value: int32; }
        extend Handle {
            destructor {}
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

    // Each one gives its source away, which needs no mark.
    CHECK(diagnostics.empty());
}

TEST_CASE("a bare owning value is consumed in every by-value context") {
    const std::vector<SemanticDiagnostic> diagnostics = AnalyzeConsumptionDiagnostics(R"(

        struct Handle { value: int32; }
        extend Handle {
            destructor {}
        }
        extend Handle {
            func Consume(self) {}
        }
        struct Owner { handle: Handle; }

        func NewHandle(value: int32) -> Handle {
            return Handle { value: value };
        }

        func Take(value: Handle) {}

        func Main(flag: bool) {
            let initialized = NewHandle(1);
            let movedByInitialization = initialized;
            initialized;

            var assigned = NewHandle(2);
            var destination = NewHandle(3);
            destination = assigned;
            assigned;

            let argument = NewHandle(4);
            Take(argument);
            argument;

            let field = NewHandle(5);
            let owner = Owner { handle: field };
            field;

            let arrayValue = NewHandle(6);
            let values = [arrayValue];
            arrayValue;

            let tupleValue = NewHandle(7);
            let tuple = (tupleValue, 1);
            tupleValue;

            let receiver = NewHandle(8);
            (receiver).Consume();
            receiver;

            var ternaryLeft = NewHandle(9);
            var ternaryRight = NewHandle(10);
            let selected = if flag { ternaryLeft } else { ternaryRight };
            ternaryLeft;
            ternaryRight;

            var reusable = NewHandle(11);
            let consumed = reusable;
            reusable = NewHandle(12);
            Take(reusable);
            reusable;

            let copy = 12;
            let copied = copy;
            copy;
        }
    )");

    const std::vector<std::string> expected = {
        "'initialized' is used after it was given away",  "'assigned' is used after it was given away",
        "'argument' is used after it was given away",     "'field' is used after it was given away",
        "'arrayValue' is used after it was given away",   "'tupleValue' is used after it was given away",
        "'receiver' is used after it was given away",     "'ternaryLeft' is used after it was given away",
        "'ternaryRight' is used after it was given away", "'reusable' is used after it was given away",
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
            destructor {}
        }

        func ReturnHandle(value: Handle) -> Handle {
            return value;
        }

        func Main() {
            let first = Handle { value: 1 };
            let second = first;
            ReturnHandle(second);
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
    const ValueConsumption *returnFact = model.TryGetConsumption(returned);
    REQUIRE(returnFact != nullptr);
    CHECK_EQ(returnFact->kind, ValueConsumptionKind::Return);
    CHECK_EQ(returnFact->type, TypeRef::MakeNamed("Handle"));

    const auto *mainFunction = dynamic_cast<const FuncDecl *>(parsed.module.items[3].get());
    const auto *second = dynamic_cast<const LetStmt *>(mainFunction->body->stmts[1].get());
    REQUIRE(second != nullptr);
    const ValueConsumption *initializationFact = model.TryGetConsumption(*second->init);
    REQUIRE(initializationFact != nullptr);
    CHECK_EQ(initializationFact->kind, ValueConsumptionKind::Initialization);

    const HirPackage package = AstToHirLowering(model).Generate();
    REQUIRE_EQ(package.modules.size(), 1);
    REQUIRE_EQ(package.modules[0].funcs.size(), 2);
    const HirFunc &loweredMain = package.modules[0].funcs[1];
    REQUIRE(loweredMain.body.has_value());
    const auto *loweredSecond = dynamic_cast<const HirLetStmt *>(loweredMain.body->stmts[1].get());
    REQUIRE(loweredSecond != nullptr);
    REQUIRE(loweredSecond->init->consumption.has_value());
    CHECK_EQ(*loweredSecond->init->consumption, ValueConsumptionKind::Initialization);
}

TEST_CASE("a generic store retains ownership transfer for its instantiation") {
    // A value of a type parameter is owning, so a bare one is transferred. The record is keyed by the generic
    // expression that every instantiation shares, so it keeps the unsubstituted type: each instantiation substitutes
    // its own type argument when lowering builds the move plan. Recording one instantiation's concrete type here would
    // hand that type's move operation to every other instantiation of the same body.
    Lexer lexer(R"(
        struct Handle { value: int32; }
        extend Handle {
            destructor {}
        }

        // Safety: `slot` must address a live, writable `T`
        func Store<T>(slot: *var T, value: T) {
            *slot = value;
        }

        func Main() {
            var room = Handle { value: 0 };
            // Safety: `@room` addresses a live local `Handle`
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
}

TEST_CASE("a match takes a named owning subject") {
    // Taking a payload out of an option and destroying the option as well would destroy the payload twice, which is
    // what every unwrap in the standard packages did. Only a subject that is a value in its own right is consumed:
    // one read through a borrow has nothing taken from it, and a pattern that binds nothing takes nothing.
    Lexer lexer(R"(
        struct Handle { value: int32; }
        extend Handle {
            destructor {}
        }
        variant Held { Full(Handle), Empty }

        func Discard(held: Held) {
            match held {
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

    const ValueConsumption *taken = model.TryGetConsumption(subjectOf(3));
    REQUIRE(taken != nullptr);
    CHECK_EQ(taken->kind, ValueConsumptionKind::MatchSubject);
    CHECK(model.TryGetConsumption(subjectOf(4)) != nullptr);
    CHECK(model.TryGetConsumption(subjectOf(5)) == nullptr);
}

TEST_CASE("a by-value match owns the copy it makes of a named plain-data subject") {
    // A match takes its subject, so the copy of plain data is a value nobody else holds and the arms own it, whether
    // or not they bind. A subject read through a pointer or a reference stays with its owner. A copy is the subject's
    // bytes, so no fact beyond the ownership is recorded for it.
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
            match held {
                .Full(_) => {},
                .Empty => {}
            }
        }

        // Safety: `held` must address a live `Held`
        func Borrowed(held: *Held) {
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
    CHECK(model.IsOwnedMatchSubject(subjectOf(4)));
    CHECK(model.IsOwnedMatchSubject(subjectOf(5)));
    CHECK(model.IsOwnedMatchSubject(subjectOf(6)));
    CHECK_FALSE(model.IsOwnedMatchSubject(subjectOf(7)));
    CHECK_FALSE(model.IsOwnedMatchSubject(subjectOf(8)));
}

TEST_CASE("assignment transfers a temporary or a named owning value and never a field") {
    // A fresh temporary and a named local or parameter transfer into the target as they stand, whether the target is a
    // local or a referent written through `&var`. A field keeps its value: it is copied with an explicit `Clone`.
    const std::vector<SemanticDiagnostic> diagnostics = AnalyzeConsumptionDiagnostics(R"(
        struct Counted { id: int32; }
        extend Counted {
            func New(id: int32) -> Counted { return Counted { id: id }; }
            func Clone(&self) -> Counted { return Counted { id: self.id }; }
            destructor {}
        }
        struct Holder { item: Counted; }
        func Make(id: int32) -> Counted { return Counted { id: id }; }

        func Assign(flag: bool, which: int32, named: Counted, holder: Holder, pointer: *Counted, slot: &var Counted) {
            var item = Make(0);
            var moved = Make(9);
            item = Make(1);
            item = Counted { id: 2 };
            item = Counted::New(3);
            item = if flag { Make(4) } else { Make(5) };
            item = match which { 6 => Make(6), _ => Make(7) };
            item = moved;
            slot = Make(8);
            item = named.Clone();
            item = holder.item.Clone();
            // Safety: callers pass `pointer` addressing a live `Counted`
            item = (*pointer).Clone();
            slot = named.Clone();
            item = holder.item;
            slot = named;
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics[0].message, "'item' is a field of 'holder' and cannot be given away by itself");
}

TEST_CASE("rejected by-value contexts do not move their operands") {
    const std::vector<SemanticDiagnostic> diagnostics = AnalyzeConsumptionDiagnostics(R"(
        struct Handle { value: int32; }
        extend Handle {
            destructor {}
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
        CHECK(diagnostic.message.find("used after it was given away") == std::string::npos);
    }
}

TEST_CASE("partial and self moves are rejected before ownership state changes") {
    const std::vector<SemanticDiagnostic> diagnostics = AnalyzeConsumptionDiagnostics(R"(
        struct Handle { value: int32; }
        extend Handle {
            destructor {}
        }
        struct Owner { handle: Handle; }

        func Take(value: Handle) {}

        // Safety: `pointer` must address a live `Handle`, which the read takes out
        func Invalid(inputHandle: Handle, inputOwner: Owner, values: Handle[2], pointer: *Handle) {
            var handle = inputHandle;
            var owner = inputOwner;
            Take(owner.handle);
            Take(values[0]);
            Take(*pointer);
            handle = handle;
            owner.handle = owner.handle;
        }
    )");

    const std::vector<std::string> expected = {
        "'handle' is a field of 'owner' and cannot be given away by itself",
        "indexed element [0] of 'values' cannot be given away by itself",
        "cannot give 'handle' to itself",
        "cannot give 'owner.handle' to itself",
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
            destructor {}
        }

        func NewHandle(value: int32) -> Handle { return Handle { value: value }; }
        func Take(value: Handle) {}

        func Valid() {
            var reused = NewHandle(1);
            Take(reused);
            reused = NewHandle(2);
            Take(reused);

            var initializedLater: Handle;
            initializedLater = NewHandle(3);
            Take(initializedLater);
        }
    )");

    CHECK(diagnostics.empty());
}
