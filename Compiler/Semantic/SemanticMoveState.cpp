#include "Semantic/Detail/MovePlace.h"
#include "Semantic/Detail/SemanticAnalyzerContext.h"

#include <algorithm>
#include <format>
#include <ranges>
#include <utility>

namespace Rux::SemanticDetail {
namespace {
std::string ExplicitMoveHelp(const ValueConsumptionKind kind, const std::string &place) {
    switch (kind) {
    case ValueConsumptionKind::Initialization:
        return std::format("write 'let destination <- {}' to transfer ownership", place);
    case ValueConsumptionKind::Assignment:
        return std::format("write 'destination <- {}' to transfer ownership", place);
    case ValueConsumptionKind::Argument:
        return std::format("prefix the argument with '<-', as in 'Take(<-{})'", place);
    case ValueConsumptionKind::Receiver:
        return std::format("prefix the receiver with '<-', as in '(<-{}).Method()'", place);
    case ValueConsumptionKind::Return:
        return std::format("prefix the return value with '<-', as in 'return <-{}'", place);
    case ValueConsumptionKind::Aggregate:
        return std::format("prefix the aggregate value with '<-', as in '{{ field: <-{} }}'", place);
    case ValueConsumptionKind::ConditionalArm:
        return std::format("prefix the selected value with '<-', as in 'condition ? <-{} : fallback'", place);
    case ValueConsumptionKind::ExplicitMove:
        break;
    }
    return std::format("prefix the value with '<-', as in '<-{}'", place);
}
} // namespace

Symbol *SemanticAnalyzerContext::Define(Symbol symbol) {
    const std::string name = symbol.name;
    if (!currentScope->Define(std::move(symbol), diags, currentFile)) {
        return nullptr;
    }
    return currentScope->LookupLocal(name);
}

Symbol *SemanticAnalyzerContext::DefineTrackedLocal(Symbol symbol, const bool initialized) {
    Symbol *defined = Define(std::move(symbol));
    if (defined) {
        // Fixed arrays are raw inline storage and may be populated element-by-element. Until element-level definite
        // initialization exists, tracking the aggregate as uninitialized produces false paths for every fill loop.
        const bool hasInitialStorage = initialized || defined->type.kind == TypeRef::Kind::Array;
        moveStates.Declare(MoveStateTracker::Local(defined),
                           hasInitialStorage ? MoveStateTracker::State::Initialized
                                             : MoveStateTracker::State::Uninitialized,
                           defined->location);
    }
    return defined;
}

const FuncDecl *SemanticAnalyzerContext::BeginTrackedFunction(const FuncDecl &function) {
    const FuncDecl *previousFunction = currentFunctionDecl;
    savedMoveStates.push_back(std::move(moveStates));
    savedTrackedFlowReachability.push_back(trackedFlowReachable);
    savedTrackedLoops.push_back(std::move(trackedLoops));
    savedActiveBorrows.push_back(std::move(activeBorrows));
    savedEndedBorrowProvenance.push_back(std::move(endedBorrowProvenance));
    savedPendingCallBorrows.push_back(std::move(pendingCallBorrows));
    savedBorrowLiveAfter.push_back(std::move(borrowLiveAfter));
    savedBorrowLastUseOffsets.push_back(std::move(borrowLastUseOffsets));
    savedBorrowStatements.push_back(currentBorrowStatement);
    moveStates.Reset();
    trackedFlowReachable = true;
    trackedLoops.clear();
    PrepareBorrowAnalysis(function);
    currentFunctionDecl = &function;
    return previousFunction;
}

void SemanticAnalyzerContext::EndTrackedFunction(const FuncDecl *previousFunction) {
    FinishBorrowAnalysis();
    currentFunctionDecl = previousFunction;
    moveStates = std::move(savedMoveStates.back());
    savedMoveStates.pop_back();
    trackedFlowReachable = savedTrackedFlowReachability.back();
    savedTrackedFlowReachability.pop_back();
    trackedLoops = std::move(savedTrackedLoops.back());
    savedTrackedLoops.pop_back();
    activeBorrows = std::move(savedActiveBorrows.back());
    savedActiveBorrows.pop_back();
    endedBorrowProvenance = std::move(savedEndedBorrowProvenance.back());
    savedEndedBorrowProvenance.pop_back();
    pendingCallBorrows = std::move(savedPendingCallBorrows.back());
    savedPendingCallBorrows.pop_back();
    borrowLiveAfter = std::move(savedBorrowLiveAfter.back());
    savedBorrowLiveAfter.pop_back();
    borrowLastUseOffsets = std::move(savedBorrowLastUseOffsets.back());
    savedBorrowLastUseOffsets.pop_back();
    currentBorrowStatement = savedBorrowStatements.back();
    savedBorrowStatements.pop_back();
}

void SemanticAnalyzerContext::CheckTrackedRead(const Symbol &symbol, const SourceLocation location) {
    if (!trackedFlowReachable) {
        return;
    }
    if (!checkingBorrowProjectionRoot) {
        CheckBorrowedRead(symbol, location);
    }
    const std::optional<MoveStateTracker::Issue> issue = moveStates.Read(MoveStateTracker::Local(&symbol));
    if (!issue) {
        return;
    }

    if (issue->kind == MoveStateTracker::IssueKind::Uninitialized) {
        EmitError(location, std::format("variable '{}' is used before it is initialized", symbol.name),
                  {std::format("'{}' was declared without an initializer at {}:{}", symbol.name,
                               issue->previousTransition.line, issue->previousTransition.column)},
                  std::format("assign a value to '{}' before this use", symbol.name));
        return;
    }
    if (issue->kind == MoveStateTracker::IssueKind::Moved) {
        EmitError(location, std::format("value '{}' is used after it was moved", symbol.name),
                  {std::format("'{}' was moved at {}:{}", symbol.name, issue->previousTransition.line,
                               issue->previousTransition.column)},
                  std::format("clone '{}' before moving it if both uses are required", symbol.name));
        return;
    }

    std::string condition = "may be unavailable";
    if (issue->kind == MoveStateTracker::IssueKind::PossiblyUninitialized) {
        condition = "may be uninitialized";
    }
    else if (issue->kind == MoveStateTracker::IssueKind::PossiblyMoved) {
        condition = "may have been moved";
    }
    EmitError(location, std::format("value '{}' {} on some control-flow paths", symbol.name, condition),
              {std::format("one unavailable path for '{}' originates at {}:{}", symbol.name,
                           issue->previousTransition.line, issue->previousTransition.column)},
              std::format("initialize or preserve '{}' on every path before this use", symbol.name));
}

SemanticAnalyzerContext::TrackedFlow SemanticAnalyzerContext::SaveTrackedFlow() const {
    return {moveStates.Save(), SaveBorrows(), trackedFlowReachable};
}

void SemanticAnalyzerContext::RestoreTrackedFlow(const TrackedFlow &flow) {
    moveStates.Restore(flow.states);
    RestoreBorrows(flow.borrows);
    trackedFlowReachable = flow.reachable;
}

void SemanticAnalyzerContext::MergeTrackedFlows(const std::vector<TrackedFlow> &flows) {
    std::vector<MoveStateTracker::Snapshot> reachable;
    std::vector<BorrowSnapshot> reachableBorrows;
    reachable.reserve(flows.size());
    for (const TrackedFlow &flow : flows) {
        if (flow.reachable) {
            reachable.push_back(flow.states);
            reachableBorrows.push_back(flow.borrows);
        }
    }
    if (reachable.empty()) {
        trackedFlowReachable = false;
        return;
    }
    moveStates.Restore(MoveStateTracker::Merge(reachable));
    RestoreBorrows(MergeBorrows(reachableBorrows));
    trackedFlowReachable = true;
}

void SemanticAnalyzerContext::BeginTrackedLoop(const std::string_view label) {
    trackedLoops.push_back({std::string(label), moveStates.Save(), SaveBorrows(), {}, {}});
}

SemanticAnalyzerContext::TrackedLoop SemanticAnalyzerContext::EndTrackedLoop() {
    TrackedLoop loop = std::move(trackedLoops.back());
    trackedLoops.pop_back();
    return loop;
}

void SemanticAnalyzerContext::RecordTrackedLoopExit(const std::string_view label, const bool isContinue) {
    auto target = trackedLoops.rbegin();
    if (!label.empty()) {
        target = std::ranges::find(trackedLoops.rbegin(), trackedLoops.rend(), label, &TrackedLoop::label);
    }
    if (target == trackedLoops.rend()) {
        return;
    }
    TrackedFlow exit = SaveTrackedFlow();
    exit.states = MoveStateTracker::Project(exit.states, target->shape);
    exit.borrows = ProjectBorrows(exit.borrows, target->borrowShape);
    (isContinue ? target->continues : target->breaks).push_back(std::move(exit));
}

TypeRef SemanticAnalyzerContext::CheckShortCircuitExpression(const BinaryExpr &expression) {
    const TypeRef left = CheckExpr(*expression.left);
    const TrackedFlow shortCircuitExit = SaveTrackedFlow();
    const TypeRef right = CheckExpr(*expression.right);
    const TrackedFlow evaluatedExit = SaveTrackedFlow();

    const auto *literal = dynamic_cast<const LiteralExpr *>(expression.left.get());
    const bool hasConstantLeft = literal && literal->token.kind == TokenKind::BoolLiteral;
    const bool evaluatesRight =
        hasConstantLeft && ((expression.op == TokenKind::AmpAmp && literal->token.text == "true") ||
                            (expression.op == TokenKind::PipePipe && literal->token.text == "false"));
    if (!hasConstantLeft) {
        MergeTrackedFlows({shortCircuitExit, evaluatedExit});
    }
    else {
        RestoreTrackedFlow(evaluatesRight ? evaluatedExit : shortCircuitExit);
    }
    return CheckBinary(expression.op, left, right, *expression.left, *expression.right, expression.location);
}

TypeRef SemanticAnalyzerContext::CheckTernaryExpression(const TernaryExpr &expression) {
    const TypeRef condition = CheckExpr(*expression.condition);
    CheckBooleanCondition(condition, expression.condition->location, "?:");
    const TrackedFlow branchEntry = SaveTrackedFlow();

    const TypeRef thenType = CheckExpr(*expression.thenExpr);
    ConsumeValue(*expression.thenExpr, thenType, ValueConsumptionKind::ConditionalArm, expression.thenExpr->location);
    const TrackedFlow thenExit = SaveTrackedFlow();

    RestoreTrackedFlow(branchEntry);
    const TypeRef elseType = CheckExpr(*expression.elseExpr);
    ConsumeValue(*expression.elseExpr, elseType, ValueConsumptionKind::ConditionalArm, expression.elseExpr->location);
    const TrackedFlow elseExit = SaveTrackedFlow();
    MergeTrackedFlows({thenExit, elseExit});
    return thenType.IsUnknown() ? elseType : thenType;
}

namespace {
/// Whether a pattern binds anything a value can be taken out into.
///
/// A wildcard or a literal reads the subject and keeps nothing; an enum pattern with a named position, a struct
/// pattern or a tuple pattern takes a piece of the subject and gives it a name. That is the difference between
/// looking at a value and taking it apart.
bool PatternBindsValue(const Pattern &pattern) {
    if (dynamic_cast<const IdentPattern *>(&pattern)) {
        return true;
    }
    if (const auto *enumeration = dynamic_cast<const EnumPattern *>(&pattern)) {
        return std::ranges::any_of(enumeration->args,
                                   [](const PatternPtr &argument) { return PatternBindsValue(*argument); }) ||
               std::ranges::any_of(enumeration->namedArgs, [](const EnumPattern::NamedArg &named) {
                   return PatternBindsValue(*named.pattern);
               });
    }
    if (const auto *structure = dynamic_cast<const StructPattern *>(&pattern)) {
        return std::ranges::any_of(structure->fields,
                                   [](const StructPattern::Field &field) { return PatternBindsValue(*field.pattern); });
    }
    if (const auto *tuple = dynamic_cast<const TuplePattern *>(&pattern)) {
        return std::ranges::any_of(tuple->elements,
                                   [](const PatternPtr &element) { return PatternBindsValue(*element); });
    }
    if (const auto *guarded = dynamic_cast<const GuardedPattern *>(&pattern)) {
        return guarded->inner && PatternBindsValue(*guarded->inner);
    }
    return false;
}
} // namespace

/// A match that binds part of its subject takes that part out of it, so the subject must not also be destroyed
/// holding what an arm now owns.
///
/// A subject read through a borrow is a different thing -- nothing is taken from it, and an `IsSome` looking at an
/// option it does not own must stay legal -- so only a subject that is a value in its own right is consumed. Called
/// before the arms are walked rather than after: each arm starts from the flow saved at the match and their exits
/// are merged over it, so a move recorded afterwards would be merged away.
template <typename Arm>
void SemanticAnalyzerContext::ConsumeMatchSubject(const Expr &subject, const TypeRef &subjectType,
                                                  const std::vector<Arm> &arms, const SourceLocation location) {
    if (!std::ranges::any_of(arms, [](const Arm &arm) { return PatternBindsValue(*arm.pattern); })) {
        return;
    }
    if (AnalyzeMovePlace(subject).IsBorrowedStorage()) {
        return;
    }
    ConsumeValue(subject, subjectType, ValueConsumptionKind::Receiver, location);
}

template void SemanticAnalyzerContext::ConsumeMatchSubject<MatchExpr::Arm>(const Expr &, const TypeRef &,
                                                                           const std::vector<MatchExpr::Arm> &,
                                                                           SourceLocation);
template void SemanticAnalyzerContext::ConsumeMatchSubject<MatchStmt::Arm>(const Expr &, const TypeRef &,
                                                                           const std::vector<MatchStmt::Arm> &,
                                                                           SourceLocation);

TypeRef SemanticAnalyzerContext::CheckMatchExpression(const MatchExpr &expression) {
    const TypeRef expressionType = CheckExpr(*expression.subject);
    const TypeRef subjectType = expressionType.kind == TypeRef::Kind::Reference && !expressionType.inner.empty()
                                  ? expressionType.inner.front()
                                  : expressionType;

    ConsumeMatchSubject(*expression.subject, expressionType, expression.arms, expression.location);

    const TrackedFlow matchEntry = SaveTrackedFlow();
    std::vector<TrackedFlow> exits;
    std::vector<const Pattern *> patterns;
    TypeRef resultType = TypeRef::MakeUnknown();
    bool coveredAll = false;

    for (const auto &arm : expression.arms) {
        RestoreTrackedFlow(matchEntry);
        PushScope();
        CheckPattern(*arm.pattern, subjectType);
        const TypeRef armType = CheckExpr(*arm.body);
        ConsumeValue(*arm.body, armType, ValueConsumptionKind::ConditionalArm, arm.location);
        PopScope();
        patterns.push_back(arm.pattern.get());
        if (!coveredAll) {
            exits.push_back(SaveTrackedFlow());
        }
        coveredAll = coveredAll || dynamic_cast<const WildcardPattern *>(arm.pattern.get()) != nullptr ||
                     dynamic_cast<const IdentPattern *>(arm.pattern.get()) != nullptr;

        if (resultType.IsUnknown()) {
            resultType = armType;
        }
        else if (!armType.IsUnknown() && !CanAssignExprTo(*arm.body, armType, resultType)) {
            EmitError(arm.location,
                      AssignmentErrorMessage(*arm.body, resultType,
                                             std::format("match arm type mismatch: expected '{}', found '{}'",
                                                         resultType.ToString(), armType.ToString())));
        }
    }
    ValidateMatchPatterns(patterns, subjectType);
    if (!MatchPatternsAreExhaustive(patterns, subjectType)) {
        exits.push_back(matchEntry);
    }
    MergeTrackedFlows(exits);
    return resultType;
}

TypeRef SemanticAnalyzerContext::ReadTrackedSymbol(const Symbol &symbol, const SourceLocation location) {
    if (symbol.kind == Symbol::Kind::Var && !checkingPlainAssignmentTarget) {
        CheckTrackedRead(symbol, location);
        ExpireBorrowAtLastUse(symbol, location);
    }
    return symbol.type;
}

void SemanticAnalyzerContext::RecordCheckedExpression(const Expr &expression, const TypeRef &type) {
    if (type.IsUnknown()) {
        // An expression lowering will ask a type for has to have one; reporting it here is what keeps that invariant a
        // diagnostic rather than a crash further down.
        ReportUntypedExpression(expression);
        return;
    }
    expressionTypes.insert_or_assign(&expression, type);
    if (!dynamic_cast<const IdentExpr *>(&expression)) {
        moveStates.Declare(MoveStateTracker::Temporary(&expression), MoveStateTracker::State::Initialized,
                           expression.location);
    }
}

void SemanticAnalyzerContext::MarkTrackedAssignment(const Expr &target, const SourceLocation location) {
    const Expr *root = &target;
    while (true) {
        if (const auto *field = dynamic_cast<const FieldExpr *>(root)) {
            root = field->object.get();
            continue;
        }
        if (const auto *index = dynamic_cast<const IndexExpr *>(root)) {
            root = index->object.get();
            continue;
        }
        break;
    }

    const auto *identifier = dynamic_cast<const IdentExpr *>(root);
    if (!identifier) {
        return;
    }
    if (Symbol *symbol = currentScope->Lookup(identifier->name); symbol && symbol->kind == Symbol::Kind::Var) {
        moveStates.Assign(MoveStateTracker::Local(symbol), location);
    }
}

std::optional<MoveStateTracker::Issue> SemanticAnalyzerContext::MoveTrackedExpression(const Expr &expression,
                                                                                      const SourceLocation location) {
    if (const auto *move = dynamic_cast<const MoveExpr *>(&expression)) {
        return MoveTrackedExpression(*move->operand, location);
    }
    if (const auto *identifier = dynamic_cast<const IdentExpr *>(&expression)) {
        if (Symbol *symbol = currentScope->Lookup(identifier->name); symbol && symbol->kind == Symbol::Kind::Var) {
            return moveStates.Move(MoveStateTracker::Local(symbol), location);
        }
    }
    if (dynamic_cast<const SelfExpr *>(&expression)) {
        if (Symbol *symbol = currentScope->Lookup("self"); symbol && symbol->kind == Symbol::Kind::Var) {
            return moveStates.Move(MoveStateTracker::Local(symbol), location);
        }
    }
    return moveStates.Move(MoveStateTracker::Temporary(&expression), location);
}

bool SemanticAnalyzerContext::ValidateMoveSource(const Expr &expression, const SourceLocation location) {
    if (!CheckBorrowedMove(expression, location)) {
        return false;
    }
    const auto usesReferenceStorage = [&](this auto &&self, const Expr &candidate) -> bool {
        const Expr *object = nullptr;
        if (const auto *field = dynamic_cast<const FieldExpr *>(&candidate)) {
            object = field->object.get();
        }
        else if (const auto *index = dynamic_cast<const IndexExpr *>(&candidate)) {
            object = index->object.get();
        }
        if (!object) {
            return false;
        }
        if (const auto found = expressionTypes.find(object);
            found != expressionTypes.end() && found->second.kind == TypeRef::Kind::Reference) {
            return true;
        }
        return self(*object);
    };
    const auto usesRawPointerStorage = [&](this auto &&self, const Expr &candidate) -> bool {
        const Expr *object = nullptr;
        if (const auto *field = dynamic_cast<const FieldExpr *>(&candidate)) {
            object = field->object.get();
        }
        else if (const auto *index = dynamic_cast<const IndexExpr *>(&candidate)) {
            object = index->object.get();
        }
        else if (const auto *unary = dynamic_cast<const UnaryExpr *>(&candidate);
                 unary && unary->op == TokenKind::Star) {
            object = unary->operand.get();
        }
        if (!object) {
            return false;
        }
        if (const auto found = expressionTypes.find(object);
            found != expressionTypes.end() && found->second.kind == TypeRef::Kind::Pointer) {
            return true;
        }
        return self(*object);
    };
    const bool rawPointerStorage = usesRawPointerStorage(expression);
    if (usesReferenceStorage(expression) && !rawPointerStorage) {
        const MovePlace place = AnalyzeMovePlace(expression);
        EmitError(location, std::format("cannot move '{}' out of borrowed reference storage", place.Display()),
                  {"references do not transfer ownership of the value they borrow"},
                  "move the owning value or clone the borrowed value explicitly");
        return false;
    }
    if (rawPointerStorage && currentFunctionDecl && !currentTypeParams.empty()) {
        // Generic owning containers cannot encode ownership in `*var T`. Requiring `<-` makes the unsafe transfer
        // visible, and restricting the exception to a generic body keeps concrete raw pointers borrowed by default.
        // An extend block contributes its aggregate's parameters to `currentTypeParams`; they are not repeated on
        // every method declaration.
        return true;
    }
    const MovePlace place = AnalyzeMovePlace(expression);
    if (place.IsBorrowedStorage()) {
        EmitError(location, std::format("cannot move '{}' out of borrowed pointer storage", place.Display()),
                  {"borrowed pointers do not transfer ownership of the value they address"},
                  "move the owning value or clone the pointed-to value explicitly");
        return false;
    }
    if (place.IsComplete()) {
        return true;
    }

    EmitError(location,
              std::format("cannot move {} out of droppable value '{}'", place.LastProjectionDescription(),
                          place.ContainerDisplay()),
              {"partial moves would leave the aggregate with only some fields initialized"},
              "move the complete aggregate or borrow or clone the component explicitly");
    return false;
}

bool SemanticAnalyzerContext::RejectSelfMove(const Expr &target, const Expr &value, const SourceLocation location) {
    if (!SameStoragePlace(target, value)) {
        return false;
    }
    const std::string place = AnalyzeMovePlace(target).Display();
    EmitError(location, std::format("cannot move '{}' into itself", place),
              {"the assignment source and destination identify the same move-only storage"},
              "remove the assignment or assign a distinct value");
    return true;
}

bool SemanticAnalyzerContext::RejectImplicitMove(const Expr &expression, const TypeRef &type,
                                                 const ValueConsumptionKind kind, const SourceLocation location) {
    const MovePlace place = AnalyzeMovePlace(expression);
    if (!place.IsNamedStorage() && !place.IsBorrowedStorage()) {
        return false;
    }
    const std::string display = place.Display();
    EmitError(
        location,
        std::format("move-only value '{}' requires an explicit '<-' in {}", display, ValueConsumptionKindName(kind)),
        {std::format("plain by-value use copies its source, but '{}' prohibits copying", type.ToString())},
        ExplicitMoveHelp(kind, display));
    return true;
}

void SemanticAnalyzerContext::ConsumeValue(const Expr &expression, const TypeRef &type, const ValueConsumptionKind kind,
                                           const SourceLocation location) {
    if (!trackedFlowReachable) {
        return;
    }
    // A `<-value` node performs and records its transfer while it is checked. The surrounding by-value context must
    // not try to consume the same source a second time.
    if (dynamic_cast<const MoveExpr *>(&expression)) {
        return;
    }
    if (!ClassifyTypeProperties(type).IsResolved() && MentionsTypeParameter(type) && currentFunctionDecl) {
        // Plain by-value use means copy, but whether a symbolic type permits that is known only at instantiation.
        // Record the question so a copyable argument receives its copy plan and a move-only one gets the explicit
        // transfer diagnostic at the generic source location.
        deferredConsumptions[currentFunctionDecl].push_back({&expression, kind, type, location});
        return;
    }
    const TypeProperties properties = ClassifyTypeProperties(type);
    if (properties.IsCopy()) {
        const MovePlace place = AnalyzeMovePlace(expression);
        const bool storedAggregate =
            type.kind == TypeRef::Kind::Named || type.kind == TypeRef::Kind::Array || type.kind == TypeRef::Kind::Tuple;
        if (storedAggregate && (place.IsNamedStorage() || place.IsBorrowedStorage())) {
            const FuncDecl *custom = nullptr;
            if (properties.copyOperation == TypeProperties::SpecialOperationState::Custom) {
                if (const FuncDecl *operation = LookupMethod(type, "=", {type}); operation && operation->body) {
                    custom = operation;
                }
            }
            valueCopies.insert_or_assign(&expression, ValueCopy{kind, type, custom, location});
        }
        return;
    }
    if (!properties.IsMoveOnly()) {
        return;
    }
    if (RejectImplicitMove(expression, type, kind, location)) {
        return;
    }
    // A fresh temporary has no named source whose later use could hide an ownership transfer, but lowering still
    // needs the consumption fact to transfer its drop flag into the destination instead of destroying both values.
    if (!properties.IsMovable()) {
        EmitError(location, std::format("moving type '{}' is prohibited", type.ToString()),
                  {"the type declares its canonical move operation without a body"},
                  "borrow the value or construct a distinct replacement instead");
        return;
    }
    if (!ValidateMoveSource(expression, location)) {
        return;
    }
    if (!MoveTrackedExpression(expression, location)) {
        valueConsumptions.insert_or_assign(
            &expression, ValueConsumption{kind, type, location, nullptr, /*constructsDestination=*/false});
    }
}

void SemanticAnalyzerContext::ConsumeExplicitValue(const Expr &expression, const TypeRef &type,
                                                   const SourceLocation location) {
    if (!trackedFlowReachable || type.IsUnknown()) {
        return;
    }
    if (type.kind == TypeRef::Kind::Reference) {
        EmitError(location, "cannot move a non-owning reference",
                  {"references borrow storage but do not own the value they address"},
                  "pass or assign the reference without '<-', or move the owning value instead");
        return;
    }
    if (!ClassifyTypeProperties(type).IsResolved() && MentionsTypeParameter(type) && currentFunctionDecl) {
        if (!ValidateMoveSource(expression, location)) {
            return;
        }
        deferredConsumptions[currentFunctionDecl].push_back(
            {&expression, ValueConsumptionKind::ExplicitMove, type, location});
        static_cast<void>(MoveTrackedExpression(expression, location));
        return;
    }
    const TypeProperties properties = ClassifyTypeProperties(type);
    if (!properties.IsMovable()) {
        EmitError(location, std::format("moving type '{}' is prohibited", type.ToString()),
                  {"the type declares its canonical move operation without a body"},
                  "borrow the value or construct a distinct replacement instead");
        return;
    }
    if (!ValidateMoveSource(expression, location)) {
        return;
    }
    if (!MoveTrackedExpression(expression, location)) {
        const MovePlace place = AnalyzeMovePlace(expression);
        const bool constructsDestination = place.IsNamedStorage();
        const FuncDecl *custom = nullptr;
        if (constructsDestination && properties.moveOperation == TypeProperties::SpecialOperationState::Custom) {
            if (const FuncDecl *operation = LookupMethod(type, "<-", {type}); operation && operation->body) {
                custom = operation;
            }
        }
        valueConsumptions.insert_or_assign(&expression, ValueConsumption{ValueConsumptionKind::ExplicitMove, type,
                                                                         location, custom, constructsDestination});
    }
}

void SemanticAnalyzerContext::ConsumeRecordedValue(const Expr &expression, const ValueConsumptionKind kind,
                                                   const SourceLocation location) {
    const auto type = expressionTypes.find(&expression);
    if (type != expressionTypes.end()) {
        ConsumeValue(expression, type->second, kind, location);
    }
}

std::vector<TypeRef> SemanticAnalyzerContext::CheckCallArgumentValues(const CallExpr &call) {
    std::vector<TypeRef> types;
    types.reserve(call.args.size());
    for (const auto &argument : call.args) {
        const auto recorded = expressionTypes.find(argument.get());
        const TypeRef type = recorded == expressionTypes.end() ? CheckExpr(*argument) : recorded->second;
        types.push_back(type);
    }
    return types;
}

void SemanticAnalyzerContext::ConsumeCallArguments(const CallExpr &call, const std::vector<TypeRef> &argumentTypes,
                                                   const std::vector<TypeRef> *parameterTypes) {
    if (parameterTypes) {
        ValidateCallReferenceBorrows(call, *parameterTypes);
    }
    const std::size_t count = std::min(call.args.size(), argumentTypes.size());
    for (std::size_t index = 0; index < count; ++index) {
        if (parameterTypes && index < parameterTypes->size() &&
            (*parameterTypes)[index].kind == TypeRef::Kind::Reference) {
            continue;
        }
        ConsumeValue(*call.args[index], argumentTypes[index], ValueConsumptionKind::Argument,
                     call.args[index]->location);
    }
}

void SemanticAnalyzerContext::ConsumeMethodReceiver(const CallExpr &call, const Expr &receiver,
                                                    const TypeRef &receiverType, const FuncDecl &method) {
    const std::optional<TypeRef> declared = ResolveMethodReceiverType(receiverType, method);
    if (declared && declared->kind == TypeRef::Kind::Reference) {
        BeginReceiverReferenceBorrow(call, receiver, *declared);
        return;
    }
    if (!declared || declared->kind == TypeRef::Kind::Pointer ||
        (declared->kind == TypeRef::Kind::Named && declared->name.starts_with("Slice<"))) {
        return;
    }
    ConsumeValue(receiver, receiverType, ValueConsumptionKind::Receiver, call.location);
}
} // namespace Rux::SemanticDetail
