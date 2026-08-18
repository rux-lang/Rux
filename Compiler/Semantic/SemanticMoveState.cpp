#include "Semantic/Detail/MovePlace.h"
#include "Semantic/Detail/SemanticAnalyzerContext.h"

#include <format>
#include <ranges>
#include <utility>

namespace Rux::SemanticDetail {
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
    moveStates.Reset();
    trackedFlowReachable = true;
    trackedLoops.clear();
    currentFunctionDecl = &function;
    return previousFunction;
}

void SemanticAnalyzerContext::EndTrackedFunction(const FuncDecl *previousFunction) {
    currentFunctionDecl = previousFunction;
    moveStates = std::move(savedMoveStates.back());
    savedMoveStates.pop_back();
    trackedFlowReachable = savedTrackedFlowReachability.back();
    savedTrackedFlowReachability.pop_back();
    trackedLoops = std::move(savedTrackedLoops.back());
    savedTrackedLoops.pop_back();
}

void SemanticAnalyzerContext::CheckTrackedRead(const Symbol &symbol, const SourceLocation location) {
    if (!trackedFlowReachable) {
        return;
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
    return {moveStates.Save(), trackedFlowReachable};
}

void SemanticAnalyzerContext::RestoreTrackedFlow(const TrackedFlow &flow) {
    moveStates.Restore(flow.states);
    trackedFlowReachable = flow.reachable;
}

void SemanticAnalyzerContext::MergeTrackedFlows(const std::vector<TrackedFlow> &flows) {
    std::vector<MoveStateTracker::Snapshot> reachable;
    reachable.reserve(flows.size());
    for (const TrackedFlow &flow : flows) {
        if (flow.reachable) {
            reachable.push_back(flow.states);
        }
    }
    if (reachable.empty()) {
        trackedFlowReachable = false;
        return;
    }
    moveStates.Restore(MoveStateTracker::Merge(reachable));
    trackedFlowReachable = true;
}

void SemanticAnalyzerContext::BeginTrackedLoop(const std::string_view label) {
    trackedLoops.push_back({std::string(label), moveStates.Save(), {}, {}});
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

TypeRef SemanticAnalyzerContext::CheckMatchExpression(const MatchExpr &expression) {
    const TypeRef subjectType = CheckExpr(*expression.subject);
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

bool SemanticAnalyzerContext::ValidateMoveSource(const Expr &expression, const TypeRef &type,
                                                 const SourceLocation location) {
    if (!ClassifyTypeProperties(type).IsMoveOnly()) {
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

bool SemanticAnalyzerContext::RejectSelfMove(const Expr &target, const Expr &value, const TypeRef &type,
                                             const SourceLocation location) {
    if (!ClassifyTypeProperties(type).IsMoveOnly() || !SameStoragePlace(target, value)) {
        return false;
    }
    const std::string place = AnalyzeMovePlace(target).Display();
    EmitError(location, std::format("cannot move '{}' into itself", place),
              {"the assignment source and destination identify the same move-only storage"},
              "remove the assignment or assign a distinct value");
    return true;
}

void SemanticAnalyzerContext::ConsumeValue(const Expr &expression, const TypeRef &type, const ValueConsumptionKind kind,
                                           const SourceLocation location) {
    if (!trackedFlowReachable || !ClassifyTypeProperties(type).IsMoveOnly()) {
        return;
    }
    if (!ValidateMoveSource(expression, type, location)) {
        return;
    }
    if (!MoveTrackedExpression(expression, location)) {
        valueConsumptions.insert_or_assign(&expression, ValueConsumption{kind, type, location});
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

void SemanticAnalyzerContext::ConsumeCallArguments(const CallExpr &call, const std::vector<TypeRef> &argumentTypes) {
    const std::size_t count = std::min(call.args.size(), argumentTypes.size());
    for (std::size_t index = 0; index < count; ++index) {
        ConsumeValue(*call.args[index], argumentTypes[index], ValueConsumptionKind::Argument,
                     call.args[index]->location);
    }
}

void SemanticAnalyzerContext::ConsumeMethodReceiver(const CallExpr &call, const Expr &receiver,
                                                    const TypeRef &receiverType, const FuncDecl &method) {
    const std::optional<TypeRef> declared = ResolveMethodReceiverType(receiverType, method);
    if (!declared || declared->kind == TypeRef::Kind::Pointer ||
        (declared->kind == TypeRef::Kind::Named && declared->name.starts_with("Slice<"))) {
        return;
    }
    ConsumeValue(receiver, receiverType, ValueConsumptionKind::Receiver, call.location);
}
} // namespace Rux::SemanticDetail
