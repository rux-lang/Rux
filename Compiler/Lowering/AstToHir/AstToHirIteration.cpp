// Iteration lowering for the types the iterator convention drives. An array, a slice or a range keeps the direct loop
// HIR already has; a user-written iterator is driven the way the convention says, by advancing it and matching what it
// reports.
//
// The iterator lives in a local of the enclosing scope, so a loop over a container allocates nothing: the container
// hands out a value, that value sits in a slot, and each iteration writes it in place.

#include "Lowering/AstToHir/Detail/AstToHirContext.h"

#include <format>
#include <utility>

namespace Rux::AstToHirDetail {
namespace {
/// The local the loop advances. The source never named it, so the spelling is one no identifier can collide with, and
/// the counter keeps nested loops apart.
[[nodiscard]] std::string IteratorBindingName(const std::size_t ordinal) {
    return std::format("$iter.{}", ordinal);
}

[[nodiscard]] std::string ItemBindingName(const std::size_t ordinal) {
    return std::format("$iter.item.{}", ordinal);
}
} // namespace

HirStmtPtr AstToHirContext::LowerIteratorFor(const ForStmt &statement, const ResolvedIteration &fact) {
    const std::size_t ordinal = iterationOrdinal++;
    const std::string iteratorName = IteratorBindingName(ordinal);
    const TypeRef iteratorType = fact.iteratorType;
    const TypeRef itemType = fact.itemType;

    // Which of the reporting enum's variants carry nothing, which is how pattern lowering tells a value with a payload
    // to read from one without.
    std::vector<std::string> unitDiscriminants;
    if (const auto declaration = enumDecls.find(fact.optionEnumName); declaration != enumDecls.end()) {
        for (const auto &variant : declaration->second->variants) {
            if (variant.fields.empty() && variant.namedFields.empty()) {
                if (auto discriminant = LookupEnumVariantDiscriminant(fact.optionEnumName, variant.name)) {
                    unitDiscriminants.push_back(*discriminant);
                }
            }
        }
    }

    auto scope = std::make_unique<HirScopeStmt>();
    scope->location = statement.location;
    scope->block.location = statement.location;

    PushScope();

    // The iterator itself: the subject when it is already one, or what the container's `Iterate` handed out.
    auto iterator = std::make_unique<HirLetStmt>();
    iterator->location = statement.location;
    iterator->isMut = true;
    iterator->name = iteratorName;
    iterator->type = iteratorType;
    HirExprPtr subject = LowerExpr(*statement.iterable);
    if (fact.kind == ResolvedIteration::Kind::Iterable) {
        iterator->init = LowerConventionCall(*fact.entry, std::move(subject), statement.location);
        iterator->type = iterator->init->type;
    }
    else {
        iterator->init = std::move(subject);
    }
    const TypeRef iteratorSlotType = iterator->type;
    HirSymbol iteratorSymbol;
    iteratorSymbol.kind = HirSymbol::Kind::Var;
    iteratorSymbol.name = iteratorName;
    iteratorSymbol.type = iteratorSlotType;
    iteratorSymbol.isMut = true;
    Define(iteratorSymbol);
    scope->block.stmts.push_back(std::move(iterator));

    auto loop = std::make_unique<HirLoopStmt>();
    loop->location = statement.location;
    loop->label = statement.label;
    loop->body.location = statement.location;

    const CleanupPlanner::LoopToken cleanupLoop = cleanupPlanner.BeginLoop(statement.label);
    PushScope();

    // `Next` reports the item or the end, so the loop body is the arm that received an item and the exit is the arm
    // that did not.
    auto advance = std::make_unique<HirVarExpr>();
    advance->location = statement.location;
    advance->name = iteratorName;
    advance->type = iteratorSlotType;
    auto reported = LowerConventionCall(*fact.advance, std::move(advance), statement.location);
    const TypeRef reportedType = reported->type;

    const std::string itemName = ItemBindingName(ordinal);
    auto itemPattern = std::make_unique<HirEnumPattern>();
    itemPattern->location = statement.location;
    itemPattern->path = {fact.optionEnumName, fact.someVariant};
    itemPattern->resolvedType = reportedType;
    itemPattern->form = CaseTypeForm::Variant;
    itemPattern->discriminant = LookupEnumVariantDiscriminant(fact.optionEnumName, fact.someVariant);
    itemPattern->hasPayload = true;
    itemPattern->payloadTypes.push_back(itemType);
    itemPattern->unitDiscriminants = unitDiscriminants;
    auto itemBinding = std::make_unique<HirBindingPattern>();
    itemBinding->location = statement.location;
    itemBinding->name = itemName;
    itemBinding->type = itemType;
    itemPattern->argIndices.push_back(0);
    itemPattern->args.push_back(std::move(itemBinding));

    // The loop variable is a fresh binding initialized from the reported item, so the body reads the name the source
    // wrote and nothing observes the temporary the pattern bound.
    HirSymbol variable;
    variable.kind = HirSymbol::Kind::Var;
    variable.name = statement.variable;
    variable.type = itemType;
    variable.bindingId = RegisterCleanupBinding(variable.name, variable.type, statement.location);
    Define(variable);

    auto item = std::make_unique<HirVarExpr>();
    item->location = statement.location;
    item->name = itemName;
    item->type = itemType;
    auto bindVariable = std::make_unique<HirLetStmt>();
    bindVariable->location = statement.location;
    bindVariable->name = statement.variable;
    bindVariable->type = itemType;
    bindVariable->bindingId = variable.bindingId;
    bindVariable->init = std::move(item);

    auto itemBody = std::make_unique<HirBlockExpr>();
    itemBody->location = statement.location;
    itemBody->type = TypeRef::MakeOpaque();
    itemBody->block.location = statement.location;
    itemBody->block.stmts.push_back(std::move(bindVariable));
    for (auto &lowered : LowerBlock(*statement.body).stmts) {
        itemBody->block.stmts.push_back(std::move(lowered));
    }

    HirMatchArm itemArm;
    itemArm.location = statement.location;
    itemArm.pattern = std::move(itemPattern);
    itemArm.body = std::move(itemBody);

    auto endPattern = std::make_unique<HirEnumPattern>();
    endPattern->location = statement.location;
    endPattern->path = {fact.optionEnumName, fact.noneVariant};
    endPattern->resolvedType = reportedType;
    endPattern->form = CaseTypeForm::Variant;
    endPattern->discriminant = LookupEnumVariantDiscriminant(fact.optionEnumName, fact.noneVariant);
    endPattern->unitDiscriminants = unitDiscriminants;

    auto exit = std::make_unique<HirBreakStmt>();
    exit->location = statement.location;
    exit->label = statement.label;
    exit->cleanups = cleanupPlanner.LoopExitActions(statement.label);
    auto endBody = std::make_unique<HirBlockExpr>();
    endBody->location = statement.location;
    endBody->type = TypeRef::MakeOpaque();
    endBody->block.location = statement.location;
    endBody->block.stmts.push_back(std::move(exit));

    HirMatchArm endArm;
    endArm.location = statement.location;
    endArm.pattern = std::move(endPattern);
    endArm.body = std::move(endBody);

    auto step = std::make_unique<HirMatchStmt>();
    step->location = statement.location;
    step->subject = std::move(reported);
    step->arms.push_back(std::move(itemArm));
    step->arms.push_back(std::move(endArm));
    loop->body.stmts.push_back(std::move(step));

    AppendCurrentScopeCleanups(loop->body);
    PopScope();
    cleanupPlanner.EndLoop(cleanupLoop);
    scope->block.stmts.push_back(std::move(loop));

    AppendCurrentScopeCleanups(scope->block);
    PopScope();
    return scope;
}

/// Call one method of the convention on a receiver the loop already has in hand. The result type comes from the
/// signature resolved here rather than from the recorded fact, because an enum's type carries its storage size and
/// every other place that constructs or matches one resolves it the same way.
HirExprPtr AstToHirContext::LowerConventionCall(const FuncDecl &method, HirExprPtr receiver,
                                                const SourceLocation location) {
    const std::string typeName = NamedBaseTypeName(receiver->type);
    auto call = std::make_unique<HirCallExpr>();
    call->location = location;
    auto callee = std::make_unique<HirVarExpr>();
    callee->location = location;
    callee->name = ConcreteMethodCalleeName(typeName, receiver->type, method);
    HirExprPtr self = LowerReceiverFor(method, std::move(receiver));
    callee->type = MethodType(self->type, method);
    call->type = callee->type.inner.empty() ? TypeRef::MakeOpaque() : callee->type.inner.back();
    call->args.push_back(std::move(self));
    call->callee = std::move(callee);
    return call;
}
} // namespace Rux::AstToHirDetail
