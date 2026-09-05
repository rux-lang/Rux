#include "MoveConsumptionTestSupport.h"

using namespace Rux;
using namespace Rux::Testing::MoveConsumptionTestSupport;

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
