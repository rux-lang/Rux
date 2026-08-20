#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
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
} // namespace

TEST_CASE("move-only values are consumed in every by-value context") {
    const std::vector<SemanticDiagnostic> diagnostics = AnalyzeConsumptionDiagnostics(R"(
        interface Drop {}

        struct Handle { value: int32; }
        extend Handle : Drop {}
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
            receiver.Consume();
            receiver;

            var ternaryLeft = NewHandle(9);
            var ternaryRight = NewHandle(10);
            let selected = flag ? ternaryLeft : ternaryRight;
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
        interface Drop {}
        struct Handle { value: int32; }
        extend Handle : Drop {}

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

    const auto *returnFunction = dynamic_cast<const FuncDecl *>(parsed.module.items[3].get());
    const auto *returnStatement = dynamic_cast<const ReturnStmt *>(returnFunction->body->stmts[0].get());
    REQUIRE(returnStatement != nullptr);
    const Expr &returned = **returnStatement->value;
    const ValueConsumption *returnFact = model.TryGetConsumption(returned);
    REQUIRE(returnFact != nullptr);
    CHECK_EQ(returnFact->kind, ValueConsumptionKind::Return);
    CHECK_EQ(returnFact->type, TypeRef::MakeNamed("Handle"));

    const auto *mainFunction = dynamic_cast<const FuncDecl *>(parsed.module.items[4].get());
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

TEST_CASE("a generic body's stores are consumed once an instantiation says what the parameter is") {
    // Whether storing a `T` hands ownership over depends on what `T` stands for, which the body cannot know. The
    // question is recorded where it is asked and answered at each instantiation; before it was, a generic container
    // kept a copy of what it was given and the caller's value was destroyed where it stood.
    Lexer lexer(R"(
        interface Drop {}
        struct Handle { value: int32; }
        extend Handle : Drop {}

        func Store<T>(slot: *var T, value: T) {
            *slot = value;
        }

        func Main() {
            var room = Handle { value: 0 };
            Store<Handle>(@room, Handle { value: 1 });
            var counted: int32 = 0;
            Store<int32>(@counted, 2i32);
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

    const auto *store = dynamic_cast<const FuncDecl *>(parsed.module.items[3].get());
    REQUIRE(store != nullptr);
    const auto *statement = dynamic_cast<const ExprStmt *>(store->body->stmts[0].get());
    REQUIRE(statement != nullptr);
    const auto *assignment = dynamic_cast<const AssignExpr *>(statement->expr.get());
    REQUIRE(assignment != nullptr);

    // One instantiation is move-only and one is not, so the fact exists and names the type that needed it.
    const ValueConsumption *fact = model.TryGetConsumption(*assignment->value);
    REQUIRE(fact != nullptr);
    CHECK_EQ(fact->kind, ValueConsumptionKind::Assignment);
    CHECK_EQ(fact->type, TypeRef::MakeNamed("Handle"));
}

TEST_CASE("rejected by-value contexts do not move their operands") {
    const std::vector<SemanticDiagnostic> diagnostics = AnalyzeConsumptionDiagnostics(R"(
        interface Drop {}
        struct Handle { value: int32; }
        extend Handle : Drop {}
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
        interface Drop {}
        struct Handle { value: int32; }
        extend Handle : Drop {}

        func Take(value: Handle) {}

        func Finish(parameter: Handle) {
            let first = Handle { value: 1 };
            if true {
                let first = Handle { value: 2 };
                first;
            }
            let last = Handle { value: 3 };
            Take(last);
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
        interface Drop {}
        struct Handle { value: int32; }
        extend Handle : Drop {}

        func Choose(parameter: Handle, selectNested: bool) -> Handle {
            let outer = Handle { value: 1 };
            if selectNested {
                let nested = Handle { value: 2 };
                return nested;
            }
            return parameter;
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
        interface Drop {}
        struct Handle { value: int32; }
        extend Handle : Drop {}

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

TEST_CASE("plain assignment schedules conditional destruction before replacing droppable storage") {
    const HirPackage package = LowerConsumptionHir(R"(
        interface Drop {}
        struct Handle { value: int32; }
        extend Handle : Drop {}
        struct Owner { handle: Handle; }

        func Replace(var destination: Handle, source: Handle) {
            destination = source;
        }

        func ReplaceField(var owner: Owner, source: Handle) {
            owner.handle = source;
        }

        func Initialize(source: Handle) {
            var destination: Handle;
            destination = source;
        }
    )");

    const HirFunc &replace = RequireFunction(package, "Replace");
    REQUIRE(replace.body.has_value());
    REQUIRE_EQ(replace.params.size(), 2);
    const auto *statement = dynamic_cast<const HirExprStmt *>(replace.body->stmts[0].get());
    REQUIRE(statement != nullptr);
    const auto *assignment = dynamic_cast<const HirAssignExpr *>(statement->expr.get());
    REQUIRE(assignment != nullptr);
    REQUIRE(assignment->overwriteCleanup.has_value());
    CHECK_EQ(assignment->overwriteCleanup->bindingId, replace.params[0].bindingId);
    CHECK_EQ(assignment->overwriteCleanup->glueSymbol, "__rux_drop__Handle");
    CHECK_EQ(assignment->value->consumedBindingId, replace.params[1].bindingId);

    const HirFunc &replaceField = RequireFunction(package, "ReplaceField");
    REQUIRE(replaceField.body.has_value());
    const auto *fieldStatement = dynamic_cast<const HirExprStmt *>(replaceField.body->stmts[0].get());
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
        interface Drop {}
        struct Handle { value: int32; }
        extend Handle : Drop {}
        struct Owner { first: Handle; copy: int32; last: Handle; }
        enum Choice { Both(Handle, Handle) }

        func Build(first: Handle, last: Handle) -> Owner {
            return Owner { first: first, copy: 1, last: last };
        }

        func Aggregate(a: Handle, b: Handle, c: Handle, d: Handle, e: Handle, f: Handle) {
            let array = [a, b];
            let tuple = (c, 1, d);
            let choice = Choice::Both(e, f);
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
        interface Drop {}
        struct Handle { value: int32; }
        extend Handle : Drop {}
        struct Owner { handle: Handle; }

        func Take(value: Handle) {}

        func Invalid(var handle: Handle, var owner: Owner, values: Handle[2], pointer: *Handle) {
            Take(owner.handle);
            Take(values[0]);
            Take(*pointer);
            handle = handle;
            owner.handle = owner.handle;
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
        interface Drop {}
        struct Handle { value: int32; }
        extend Handle : Drop {}

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
