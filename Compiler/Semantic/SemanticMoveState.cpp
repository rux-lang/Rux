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
    return moveStates.Move(MoveStateTracker::Temporary(&expression), location);
}
} // namespace Rux::SemanticDetail
