#include "Semantic/Analysis/AnalysisContext.h"
#include "Semantic/Analysis/MovePlace.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
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
    case ValueConsumptionKind::ArrayRepeat:
        return "write the elements explicitly or initialize move-only elements in a loop";
    case ValueConsumptionKind::ConditionalArm:
        return std::format("prefix the selected value with '<-', as in 'condition ? <-{} : fallback'", place);
    case ValueConsumptionKind::PropagationOperand:
        return std::format("prefix the outcome with '<-', as in '(<-{})?'", place);
    case ValueConsumptionKind::CoalescingOperand:
        return std::format("prefix the option with '<-', as in '(<-{}) ?? fallback'", place);
    case ValueConsumptionKind::CoalescingFallback:
        return std::format("prefix the fallback with '<-', as in 'option ?? <-{}'", place);
    case ValueConsumptionKind::MatchSubject:
        return std::format("transfer the subject with 'match <-{}'", place);
    case ValueConsumptionKind::CatchSubject:
        return std::format("transfer the subject with '(<-{}) catch {{ ... }}'", place);
    case ValueConsumptionKind::ConstructorOperand:
        return std::format("prefix the operand with '<-', as in '.Success(<-{})'", place);
    case ValueConsumptionKind::ExplicitMove:
        break;
    }
    return std::format("prefix the value with '<-', as in '<-{}'", place);
}
} // namespace

Symbol *AnalysisContext::Define(Symbol symbol) {
    const std::string name = symbol.name;
    if (!currentScope->Define(std::move(symbol), diags, currentFile)) {
        return nullptr;
    }
    return currentScope->LookupLocal(name);
}

bool AnalysisContext::MayNeedDestruction(const TypeRef &type) {
    if (type.IsUnknown()) {
        return false;
    }
    // A type that still depends on a type parameter may be instantiated with one that needs destruction.
    const TypeProperties properties = ClassifyTypeProperties(type);
    return properties.droppable || !properties.IsResolved();
}

Symbol *AnalysisContext::DefineTrackedLocal(Symbol symbol, const bool initialized) {
    Symbol *defined = Define(std::move(symbol));
    if (defined) {
        // Fixed arrays are raw inline storage and may be populated element-by-element. Until element-level definite
        // initialization exists, tracking the aggregate as uninitialized produces false paths for every fill loop. An
        // array whose elements need destruction cannot be filled that way, because its drop flag covers it whole and
        // a written element would never be destroyed, so it is tracked like any other local declared without a value.
        const bool fillableArray = defined->type.kind == TypeRef::Kind::Array && !MayNeedDestruction(defined->type);
        const bool hasInitialStorage = initialized || fillableArray;
        const MoveStateTracker::Identity identity = MoveStateTracker::Local(defined);
        moveStates.Declare(
            identity, hasInitialStorage ? MoveStateTracker::State::Initialized : MoveStateTracker::State::Uninitialized,
            defined->location);
        // The same holds for such an array inside a value written one part at a time.
        if (!hasInitialStorage && !MayNeedDestruction(defined->type)) {
            std::vector<std::string> arrays;
            FillableArrayParts(defined->type, "", arrays);
            for (std::string &array : arrays) {
                moveStates.AssignPart(identity, std::move(array));
            }
            if (const MoveStateTracker::Record *record = moveStates.TryGet(identity);
                record && !record->writtenParts.empty() && PartsMakeWhole(defined->type, record->writtenParts, "")) {
                moveStates.Assign(identity, defined->location);
            }
        }
    }
    return defined;
}

const FuncDecl *AnalysisContext::BeginTrackedFunction(const FuncDecl &function) {
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

void AnalysisContext::EndTrackedFunction(const FuncDecl *previousFunction) {
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

void AnalysisContext::CheckTrackedRead(const Symbol &symbol, const SourceLocation location,
                                       const std::optional<std::string_view> part) {
    if (!trackedFlowReachable) {
        return;
    }
    if (!checkingBorrowProjectionRoot) {
        CheckBorrowedRead(symbol, location);
    }
    const MoveStateTracker::Identity identity = MoveStateTracker::Local(&symbol);
    const std::optional<MoveStateTracker::Issue> issue =
        part ? moveStates.ReadPart(identity, *part) : moveStates.Read(identity);
    if (!issue) {
        return;
    }

    // A local written one part at a time says which parts it holds, since those alone may be read so far.
    std::optional<std::string> partsNote;
    if (const MoveStateTracker::Record *record = moveStates.TryGet(identity); record && !record->writtenParts.empty()) {
        std::string written;
        for (const std::string &writtenPart : record->writtenParts) {
            written += std::format("{}'{}.{}'", written.empty() ? "" : ", ", symbol.name, writtenPart);
        }
        partsNote = std::format("only {} {} been written", written, record->writtenParts.size() == 1 ? "has" : "have");
        if (part) {
            *partsNote += std::format(", not '{}.{}'", symbol.name, *part);
        }
    }
    const auto withParts = [&](std::string note) {
        std::vector<std::string> notes{std::move(note)};
        if (partsNote) {
            notes.push_back(*partsNote);
        }
        return notes;
    };

    if (issue->kind == MoveStateTracker::IssueKind::Uninitialized) {
        EmitError(location, std::format("variable '{}' is used before it is initialized", symbol.name),
                  withParts(std::format("'{}' was declared without an initializer at {}:{}", symbol.name,
                                        issue->previousTransition.line, issue->previousTransition.column)),
                  std::format("assign a value to '{}' before this use", symbol.name));
        return;
    }
    if (issue->kind == MoveStateTracker::IssueKind::Moved) {
        EmitError(location, std::format("value '{}' is used after it was moved", symbol.name),
                  withParts(std::format("'{}' was moved at {}:{}", symbol.name, issue->previousTransition.line,
                                        issue->previousTransition.column)),
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
              withParts(std::format("one unavailable path for '{}' originates at {}:{}", symbol.name,
                                    issue->previousTransition.line, issue->previousTransition.column)),
              std::format("initialize or preserve '{}' on every path before this use", symbol.name));
}

void AnalysisContext::FillableArrayParts(const TypeRef &type, const std::string &prefix,
                                         std::vector<std::string> &parts) {
    const auto child = [&](const std::string &name) { return prefix.empty() ? name : prefix + "." + name; };
    if (type.kind == TypeRef::Kind::Array) {
        if (!prefix.empty()) {
            parts.push_back(prefix);
        }
        return;
    }
    if (type.kind == TypeRef::Kind::Tuple) {
        for (std::size_t index = 0; index < type.inner.size(); ++index) {
            FillableArrayParts(type.inner[index], child(std::to_string(index)), parts);
        }
        return;
    }
    if (const auto structure = structDecls.find(NamedBaseTypeName(type)); structure != structDecls.end()) {
        for (const StructDecl::Field &field : structure->second->fields) {
            FillableArrayParts(StructFieldType(type, field.name), child(field.name), parts);
        }
    }
}

bool AnalysisContext::PartsMakeWhole(const TypeRef &type, const std::span<const std::string> parts,
                                     const std::string &prefix) {
    if (!prefix.empty() && MoveStateTracker::PartsCover(parts, prefix)) {
        return true;
    }
    const auto child = [&](const std::string &name) { return prefix.empty() ? name : prefix + "." + name; };
    if (type.kind == TypeRef::Kind::Tuple) {
        if (type.inner.empty()) {
            return false;
        }
        for (std::size_t index = 0; index < type.inner.size(); ++index) {
            if (!PartsMakeWhole(type.inner[index], parts, child(std::to_string(index)))) {
                return false;
            }
        }
        return true;
    }
    const std::string name = NamedBaseTypeName(type);
    if (const auto structure = structDecls.find(name); structure != structDecls.end()) {
        const auto &fields = structure->second->fields;
        return !fields.empty() && std::ranges::all_of(fields, [&](const StructDecl::Field &field) {
            return PartsMakeWhole(StructFieldType(type, field.name), parts, child(field.name));
        });
    }
    // The members of a union share their storage, so writing one writes the union.
    if (const auto unionType = unionDecls.find(name); unionType != unionDecls.end()) {
        return std::ranges::any_of(unionType->second->fields, [&](const UnionDecl::Field &field) {
            return MoveStateTracker::PartsCover(parts, child(field.name));
        });
    }
    return false;
}

AnalysisContext::TrackedFlow AnalysisContext::SaveTrackedFlow() const {
    return {moveStates.Save(), SaveBorrows(), trackedFlowReachable};
}

void AnalysisContext::RestoreTrackedFlow(const TrackedFlow &flow) {
    moveStates.Restore(flow.states);
    RestoreBorrows(flow.borrows);
    trackedFlowReachable = flow.reachable;
}

void AnalysisContext::MergeTrackedFlows(const std::vector<TrackedFlow> &flows) {
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

void AnalysisContext::BeginTrackedLoop(const std::string_view label) {
    trackedLoops.push_back({std::string(label), moveStates.Save(), SaveBorrows(), {}, {}});
}

AnalysisContext::TrackedLoop AnalysisContext::EndTrackedLoop() {
    TrackedLoop loop = std::move(trackedLoops.back());
    trackedLoops.pop_back();
    return loop;
}

AnalysisContext::TrackedLoop AnalysisContext::CheckTrackedLoopBody(const Block &body, const std::string_view label,
                                                                   const Symbol *rebound) {
    const TrackedFlow entry = SaveTrackedFlow();
    // The expressions a pass records, so a second pass can check them afresh rather than reuse their recorded types.
    loopCheckedExpressions.emplace_back();
    const auto checkPass = [&]() {
        BeginTrackedLoop(label);
        ++loopDepth;
        CheckBlock(body);
        --loopDepth;
        return EndTrackedLoop();
    };
    TrackedLoop loop = checkPass();
    std::vector<const Expr *> checked = std::move(loopCheckedExpressions.back());
    loopCheckedExpressions.pop_back();
    // An enclosing loop's second pass has to forget these too.
    if (!loopCheckedExpressions.empty()) {
        loopCheckedExpressions.back().insert(loopCheckedExpressions.back().end(), checked.begin(), checked.end());
    }
    if (!entry.reachable) {
        return loop;
    }

    // The next pass starts from the end of this one, or from any `continue`, as well as from the loop's entry.
    std::vector<MoveStateTracker::Snapshot> heads = {entry.states};
    if (trackedFlowReachable) {
        heads.push_back(MoveStateTracker::Project(moveStates.Save(), loop.shape));
    }
    for (const TrackedFlow &next : loop.continues) {
        if (next.reachable) {
            heads.push_back(next.states);
        }
    }
    MoveStateTracker::Snapshot head = MoveStateTracker::Merge(heads);
    if (rebound) {
        const auto identity = MoveStateTracker::Local(rebound);
        const auto atHead = std::ranges::find(head.entries, identity, &MoveStateTracker::SnapshotEntry::identity);
        const auto atEntry =
            std::ranges::find(entry.states.entries, identity, &MoveStateTracker::SnapshotEntry::identity);
        if (atHead != head.entries.end() && atEntry != entry.states.entries.end()) {
            atHead->record = atEntry->record;
        }
    }
    const bool worsened = std::ranges::any_of(entry.states.entries, [&](const MoveStateTracker::SnapshotEntry &before) {
        const auto after = std::ranges::find(head.entries, before.identity, &MoveStateTracker::SnapshotEntry::identity);
        // Only a local the loop entered with a value and a later pass may find without one is new: anything already
        // unavailable on entry was reported by the first pass.
        if (after == head.entries.end() || after->record.state == MoveStateTracker::State::Initialized) {
            return false;
        }
        // A local written one part at a time is also worse when a later pass finds fewer of its parts written.
        return before.record.state == MoveStateTracker::State::Initialized ||
               std::ranges::any_of(before.record.writtenParts, [&](const std::string &part) {
                   return !MoveStateTracker::PartsCover(after->record.writtenParts, part);
               });
    });
    if (!worsened) {
        return loop;
    }

    // A local only an earlier pass moved is used again by this one. The first check already reported everything else
    // in the body, so only what the merged state adds is kept.
    const std::size_t before = diags.size();
    TrackedFlow merged = entry;
    merged.states = head;
    RestoreTrackedFlow(merged);
    for (const Expr *expression : checked) {
        expressionTypes.erase(expression);
    }
    TrackedLoop second = checkPass();
    const auto repeated = [&](const SemanticDiagnostic &diagnostic) {
        return std::ranges::any_of(
            diags.begin(), diags.begin() + static_cast<std::ptrdiff_t>(before), [&](const SemanticDiagnostic &earlier) {
                return earlier.sourceName == diagnostic.sourceName &&
                       earlier.location.line == diagnostic.location.line &&
                       earlier.location.column == diagnostic.location.column && earlier.message == diagnostic.message;
            });
    };
    const auto removed =
        std::ranges::remove_if(diags.begin() + static_cast<std::ptrdiff_t>(before), diags.end(), repeated);
    diags.erase(removed.begin(), removed.end());
    return second;
}

void AnalysisContext::RecordTrackedLoopExit(const std::string_view label, const bool isContinue) {
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

TypeRef AnalysisContext::CheckShortCircuitExpression(const BinaryExpr &expression) {
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

TypeRef AnalysisContext::CheckTernaryExpression(const TernaryExpr &expression) {
    const TypeRef condition = ReadBorrowedScalar(*expression.condition, CheckExpr(*expression.condition));
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
    // As with match arms, a diverging branch never decides the type, and the two values must agree: the else value
    // converts to the then type, or the then value to the else type. A branch whose native type is still open, such as
    // `none`, is left to the expected type of the context.
    const TypeRef thenValue = IsDivergingExpression(*expression.thenExpr) ? TypeRef::MakeUnknown() : thenType;
    const TypeRef elseValue = IsDivergingExpression(*expression.elseExpr) ? TypeRef::MakeUnknown() : elseType;
    if (thenValue.IsUnknown()) {
        return elseValue.IsUnknown() ? thenType : elseValue;
    }
    if (elseValue.IsUnknown() || CanAssignExprTo(*expression.elseExpr, elseValue, thenValue)) {
        return thenValue;
    }
    if (CanAssignExprTo(*expression.thenExpr, thenValue, elseValue)) {
        return elseValue;
    }
    // Two integer or two floating-point branches are typed together by the context, which may hold an unsuffixed
    // literal that fits only there, such as a `uint128` bound.
    const bool contextualNumbers =
        (thenValue.IsInteger() && elseValue.IsInteger()) ||
        (thenValue.IsNumeric() && !thenValue.IsInteger() && elseValue.IsNumeric() && !elseValue.IsInteger());
    if (!contextualNumbers && !thenValue.MentionsIncompleteNative() && !elseValue.MentionsIncompleteNative()) {
        EmitError(expression.elseExpr->location,
                  std::format("conditional branch type mismatch: expected '{}', found '{}'", thenValue.DisplayString(),
                              elseValue.DisplayString()),
                  {}, "make both branches produce the same type");
        return TypeRef::MakeUnknown();
    }
    return thenValue;
}

/// Whether a pattern binds anything a value can be taken out into.
///
/// A wildcard or a literal reads the subject and keeps nothing; an enum pattern with a named position, a struct
/// pattern or a tuple pattern takes a piece of the subject and gives it a name. That is the difference between
/// looking at a value and taking it apart.
bool PatternBindsValue(const Pattern &pattern) {
    if (dynamic_cast<const IdentPattern *>(&pattern)) {
        return true;
    }
    // `v: T` binds its selection, while `_: T` only selects; `p?` binds whatever `p` binds.
    if (const auto *typed = dynamic_cast<const TypedPattern *>(&pattern)) {
        return !typed->name.empty();
    }
    if (const auto *presence = dynamic_cast<const PresencePattern *>(&pattern)) {
        return presence->inner && PatternBindsValue(*presence->inner);
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

/// A match that binds part of its subject takes that part out of it, so the subject must not also be destroyed
/// holding what an arm now owns.
///
/// A subject read through a borrow is a different thing -- nothing is taken from it, and an `IsSome` looking at an
/// option it does not own must stay legal -- so only a subject that is a value in its own right is consumed. Called
/// before the arms are walked rather than after: each arm starts from the flow saved at the match and their exits
/// are merged over it, so a move recorded afterwards would be merged away.
///
/// The arms own the subject they are handed: one transferred with `<-`, a temporary nobody else holds, or — for a
/// named copyable subject — the copy the match makes of it, which is a temporary of its own. Each arm then owns what
/// it binds and destroys what it leaves, so every value the match creates is destroyed exactly once.
template <typename Arm>
bool AnalysisContext::ConsumeMatchSubject(const Expr &subject, const TypeRef &subjectType, const std::vector<Arm> &arms,
                                          const SourceLocation location) {
    // `<-value` records its transfer on the moved operand rather than on itself.
    if (const auto *move = dynamic_cast<const MoveExpr *>(&subject)) {
        const bool handedOver = valueConsumptions.contains(move->operand.get());
        if (handedOver) {
            ownedMatchSubjects.insert(&subject);
        }
        return handedOver;
    }
    const MovePlace place = AnalyzeMovePlace(subject);
    if (subjectType.kind == TypeRef::Kind::Reference || place.IsBorrowedStorage()) {
        return false;
    }
    // A named subject that no arm takes anything from stays with its owner; a temporary has no other owner, so its
    // match takes it whole and destroys whatever the selected arm leaves.
    if (!std::ranges::any_of(arms, [](const Arm &arm) { return PatternBindsValue(*arm.pattern); }) &&
        place.IsNamedStorage()) {
        return false;
    }
    if (!trackedFlowReachable) {
        return false;
    }
    ConsumeValue(subject, subjectType, ValueConsumptionKind::MatchSubject, location);
    ownedMatchSubjects.insert(&subject);
    return true;
}

template bool AnalysisContext::ConsumeMatchSubject<MatchExpr::Arm>(const Expr &, const TypeRef &,
                                                                   const std::vector<MatchExpr::Arm> &, SourceLocation);
template bool AnalysisContext::ConsumeMatchSubject<MatchStmt::Arm>(const Expr &, const TypeRef &,
                                                                   const std::vector<MatchStmt::Arm> &, SourceLocation);

void AnalysisContext::ReportNonExhaustiveMatchExpression(const MatchExpr &expression,
                                                         const std::vector<const Pattern *> &patterns,
                                                         const TypeRef &subjectType) {
    if (subjectType.IsBool()) {
        bool hasTrue = false;
        bool hasFalse = false;
        for (const Pattern *pattern : patterns) {
            const auto *literal = dynamic_cast<const LiteralPattern *>(pattern);
            hasTrue = hasTrue || (literal && literal->value.text == "true");
            hasFalse = hasFalse || (literal && literal->value.text == "false");
        }
        const std::string missing = !hasTrue && !hasFalse ? "true, false" : !hasTrue ? "true" : "false";
        EmitError(expression.location,
                  std::format("match on '{}' is not exhaustive; missing {}", subjectType.ToString(), missing), {},
                  std::format("add an arm for {}, or an 'else' arm", missing));
        return;
    }
    if (subjectType.IsInteger()) {
        EmitError(
            expression.location,
            std::format("match on '{}' is not exhaustive; its arms do not cover every value", subjectType.ToString()),
            {}, "add an 'else' arm");
        return;
    }
    if (subjectType.kind == TypeRef::Kind::Tuple && !patterns.empty()) {
        if (const auto found = tupleMatchMissing.find(patterns.front());
            found != tupleMatchMissing.end() && found->second) {
            EmitError(
                expression.location,
                std::format("match on '{}' is not exhaustive; missing {}", subjectType.ToString(), *found->second), {},
                "add arms for the missing values, or an 'else' arm");
        }
    }
}

TypeRef AnalysisContext::CheckMatchExpression(const MatchExpr &expression) {
    const TypeRef expressionType = CheckExpr(*expression.subject);
    const TypeRef subjectType = expressionType.kind == TypeRef::Kind::Reference && !expressionType.inner.empty()
                                  ? expressionType.inner.front()
                                  : expressionType;

    const bool armsTakeParts =
        ConsumeMatchSubject(*expression.subject, expressionType, expression.arms, expression.location);
    const PatternBorrow armBorrow = IsNativeMatchSubject(subjectType)
                                      ? MatchSubjectBorrow(*expression.subject, expressionType)
                                      : PatternBorrow::Owned;

    const TrackedFlow matchEntry = SaveTrackedFlow();
    std::vector<TrackedFlow> exits;
    std::vector<const Pattern *> patterns;
    TypeRef resultType = TypeRef::MakeUnknown();
    bool coveredAll = false;

    // Arms accepted while the result was still incomplete, such as a leading `none`. A later arm that completes the
    // type has to accept them too.
    struct AcceptedArm {
        const Expr *body;
        TypeRef type;
        SourceLocation location;
    };

    std::vector<AcceptedArm> acceptedArms;
    const auto reportMismatch = [&](const Expr &body, const TypeRef &armType, const SourceLocation location) {
        EmitError(location, AssignmentErrorMessage(body, resultType,
                                                   std::format("match arm type mismatch: expected '{}', found '{}'",
                                                               resultType.ToString(), armType.DisplayString())));
    };

    for (const auto &arm : expression.arms) {
        RestoreTrackedFlow(matchEntry);
        PushScope();
        const PatternBorrow savedBorrow = std::exchange(currentPatternBorrow, armBorrow);
        const bool savedTakesParts = std::exchange(currentPatternTakesParts, armsTakeParts);
        CheckPattern(*arm.pattern, subjectType);
        currentPatternTakesParts = savedTakesParts;
        currentPatternBorrow = savedBorrow;
        // A diverging arm, such as `return` or a call to `Panic`, leaves the match and never decides its type.
        const TypeRef checkedArm = CheckExpr(*arm.body);
        const TypeRef armType = IsDivergingExpression(*arm.body) ? TypeRef::MakeUnknown() : checkedArm;
        ConsumeValue(*arm.body, armType, ValueConsumptionKind::ConditionalArm, arm.location);
        PopScope();
        patterns.push_back(arm.pattern.get());
        if (!coveredAll) {
            exits.push_back(SaveTrackedFlow());
        }
        coveredAll = coveredAll || dynamic_cast<const WildcardPattern *>(arm.pattern.get()) != nullptr ||
                     dynamic_cast<const IdentPattern *>(arm.pattern.get()) != nullptr;

        if (armType.IsUnknown()) {
            continue;
        }
        if (resultType.IsUnknown()) {
            resultType = armType;
        }
        else if (CanAssignExprTo(*arm.body, armType, resultType)) {
            // The arm fits the type the earlier arms settled on.
        }
        else if (resultType.MentionsIncompleteNative() && !armType.MentionsIncompleteNative()) {
            // `none`, `.Success(v)` and `.Failure(e)` leave part of their type open, so the first arm that has a whole
            // type decides the match, and the open arms before it are checked against that type.
            resultType = armType;
            for (const AcceptedArm &earlier : acceptedArms) {
                if (!CanAssignExprTo(*earlier.body, earlier.type, resultType)) {
                    reportMismatch(*earlier.body, earlier.type, earlier.location);
                }
            }
        }
        else if (!resultType.MentionsIncompleteNative() && !armType.MentionsIncompleteNative()) {
            reportMismatch(*arm.body, armType, arm.location);
        }
        // Otherwise no arm so far has a whole type, as with `.Success(v)` beside `.Failure(e)`. The match keeps its
        // open type and the expected type of its context checks every arm.
        acceptedArms.push_back({arm.body.get(), armType, arm.location});
    }
    ValidateMatchPatterns(patterns, subjectType);
    if (MatchPatternsAreExhaustive(patterns, subjectType)) {
        // A native subject's tag is the compiler's own and never holds a value outside its members.
        if (!IsNativeMatchSubject(subjectType)) {
            exhaustiveMatchExpressions.insert(&expression);
        }
    }
    else {
        // An expression has to produce a value whatever the subject holds. Enums and native subjects report their
        // missing cases through pattern validation; a bool or an integer has its own values to name here.
        ReportNonExhaustiveMatchExpression(expression, patterns, subjectType);
        exits.push_back(matchEntry);
    }
    MergeTrackedFlows(exits);
    return resultType;
}

TypeRef AnalysisContext::ReadTrackedSymbol(const Symbol &symbol, const SourceLocation location) {
    // The path belongs to this read alone, so it is taken before anything else can be read.
    const std::optional<std::string> part = std::exchange(partReadPath, std::nullopt);
    if (symbol.kind == Symbol::Kind::Const && symbol.declaration) {
        return CheckNamedConstant(*static_cast<const ConstDecl *>(symbol.declaration));
    }
    if (symbol.kind == Symbol::Kind::Var && !checkingPlainAssignmentTarget) {
        readSymbols.insert(&symbol);
        CheckTrackedRead(symbol, location, part ? std::optional<std::string_view>(*part) : std::nullopt);
        ExpireBorrowAtLastUse(symbol, location);
    }
    return symbol.type;
}

void AnalysisContext::RecordCheckedExpression(const Expr &expression, const TypeRef &type) {
    NoteDeferredNativeType(type);
    if (type.IsUnknown()) {
        // An expression lowering will ask a type for has to have one; reporting it here is what keeps that invariant a
        // diagnostic rather than a crash further down.
        ReportUntypedExpression(expression);
        return;
    }
    expressionTypes.insert_or_assign(&expression, type);
    if (!loopCheckedExpressions.empty()) {
        loopCheckedExpressions.back().push_back(&expression);
    }
    if (!dynamic_cast<const IdentExpr *>(&expression)) {
        moveStates.Declare(MoveStateTracker::Temporary(&expression), MoveStateTracker::State::Initialized,
                           expression.location);
    }
}

void AnalysisContext::MarkTrackedAssignment(const Expr &target, const SourceLocation location) {
    // Walk out to the local, collecting the fields between it and the place, innermost first. An element write writes
    // the array it is in, so the path stops at the first index from the local.
    std::vector<std::string> fields;
    bool leavesStorage = false;
    const auto leaves = [&](const Expr &object) {
        const auto objectType = expressionTypes.find(&object);
        return objectType != expressionTypes.end() &&
               (objectType->second.kind == TypeRef::Kind::Pointer ||
                objectType->second.kind == TypeRef::Kind::Reference || SliceElementType(objectType->second));
    };
    const Expr *root = &target;
    while (true) {
        if (const auto *field = dynamic_cast<const FieldExpr *>(root)) {
            leavesStorage = leavesStorage || leaves(*field->object);
            fields.push_back(field->field);
            root = field->object.get();
            continue;
        }
        if (const auto *index = dynamic_cast<const IndexExpr *>(root)) {
            // An operator index assigns nothing back to the object, so the walk stops rather than marking it written.
            if (IsIndexOperatorCall(*index)) {
                return;
            }
            leavesStorage = leavesStorage || leaves(*index->object);
            fields.clear();
            root = index->object.get();
            continue;
        }
        break;
    }

    const auto *identifier = dynamic_cast<const IdentExpr *>(root);
    if (!identifier) {
        return;
    }
    Symbol *symbol = currentScope->Lookup(identifier->name);
    if (!symbol || symbol->kind != Symbol::Kind::Var) {
        return;
    }
    const MoveStateTracker::Identity identity = MoveStateTracker::Local(symbol);
    if (root == &target) {
        moveStates.Assign(identity, location);
        return;
    }
    // A write through a reference, a pointer, or a slice lands outside the local's own storage.
    if (leavesStorage) {
        return;
    }
    // A part write into a type that needs destruction is already an error, and an element write into a local array
    // writes the array; either way the local is taken as written whole.
    if (fields.empty() || MayNeedDestruction(symbol->type)) {
        moveStates.Assign(identity, location);
        return;
    }
    // Each enclosing part, from the local out to the written one, with its type: a part whose own parts are now all
    // written counts as written whole, so `s.start` reads once `s.start.x` and `s.start.y` do.
    std::vector<std::pair<std::string, TypeRef>> enclosing;
    std::string path;
    TypeRef partType = symbol->type;
    for (const std::string &field : std::views::reverse(fields)) {
        enclosing.emplace_back(path, partType);
        path += (path.empty() ? "" : ".") + field;
        if (partType.kind == TypeRef::Kind::Tuple) {
            std::size_t index = 0;
            const auto [end, error] = std::from_chars(field.data(), field.data() + field.size(), index);
            partType = error == std::errc{} && end == field.data() + field.size() && index < partType.inner.size()
                         ? partType.inner[index]
                         : TypeRef::MakeUnknown();
        }
        else {
            partType = StructFieldType(partType, field);
        }
    }
    moveStates.AssignPart(identity, std::move(path));
    for (const auto &[prefix, type] : std::views::reverse(enclosing)) {
        const MoveStateTracker::Record *record = moveStates.TryGet(identity);
        if (!record || record->state == MoveStateTracker::State::Initialized ||
            !PartsMakeWhole(type, record->writtenParts, prefix)) {
            break;
        }
        if (prefix.empty()) {
            moveStates.Assign(identity, location);
        }
        else {
            moveStates.AssignPart(identity, prefix);
        }
    }
}

void AnalysisContext::ReportPartWriteIntoEmptyLocal(const Expr &target, const std::string_view action,
                                                    const SourceLocation location) {
    if (!trackedFlowReachable) {
        return;
    }
    // Walk out from the place through the projections that stay inside one value to the local they are part of. A
    // reference, a slice, or a raw pointer on the way leaves the local's own storage, so the write lands elsewhere.
    const Expr *place = &target;
    while (true) {
        const Expr *object = nullptr;
        if (const auto *field = dynamic_cast<const FieldExpr *>(place)) {
            object = field->object.get();
        }
        else if (const auto *index = dynamic_cast<const IndexExpr *>(place); index && !IsIndexOperatorCall(*index)) {
            object = index->object.get();
        }
        if (!object) {
            break;
        }
        const auto objectType = expressionTypes.find(object);
        if (objectType == expressionTypes.end() || objectType->second.kind == TypeRef::Kind::Pointer ||
            objectType->second.kind == TypeRef::Kind::Reference || SliceElementType(objectType->second)) {
            return;
        }
        place = object;
    }
    if (place == &target) {
        return;
    }
    const auto *identifier = dynamic_cast<const IdentExpr *>(place);
    if (!identifier) {
        return;
    }
    const Symbol *symbol = currentScope->Lookup(identifier->name);
    if (!symbol || symbol->kind != Symbol::Kind::Var) {
        return;
    }
    const MoveStateTracker::Record *record = moveStates.TryGet(MoveStateTracker::Local(symbol));
    if (!record || record->state == MoveStateTracker::State::Initialized) {
        return;
    }
    // A part of a local holds a value only while the local owns one, and the local's drop flag says whether it does.
    // Writing one part cannot make the whole local live, so a value written there would never be destroyed. A type
    // with nothing to destroy cannot leak, and may still be initialized part by part.
    if (!MayNeedDestruction(symbol->type)) {
        return;
    }

    using State = MoveStateTracker::State;
    const State state = record->state;
    const SourceLocation transition = record->previousTransition;
    const bool certain = state == State::Uninitialized || state == State::Moved;
    std::string note;
    switch (state) {
    case State::Uninitialized:
        note =
            std::format("'{}' was declared without a value at {}:{}", symbol->name, transition.line, transition.column);
        break;
    case State::Moved:
        note = std::format("'{}' was moved at {}:{}", symbol->name, transition.line, transition.column);
        break;
    case State::MaybeMoved:
        note = std::format("'{}' may have been moved on some control-flow paths; one originates at {}:{}", symbol->name,
                           transition.line, transition.column);
        break;
    default:
        note = std::format("'{}' may hold no value on some control-flow paths; one originates at {}:{}", symbol->name,
                           transition.line, transition.column);
        break;
    }

    const MovePlace part = AnalyzeMovePlace(target);
    const std::string container = part.ContainerDisplay();
    std::string message = std::format("cannot {} {} of '{}', ", action, part.LastProjectionDescription(), container);
    if (container == symbol->name) {
        message += certain ? "which holds no value" : "which may hold no value";
    }
    else {
        message += std::format("because '{}' {}", symbol->name, certain ? "holds no value" : "may hold no value");
    }

    // A generic instantiation is recorded under its package-qualified name; the source names it without the qualifier.
    const TypeRef &type = symbol->type;
    std::string typeName = type.DisplayString();
    if (type.kind == TypeRef::Kind::Named) {
        const std::size_t arguments = typeName.find('<');
        if (const std::size_t qualifier = typeName.rfind("::", arguments); qualifier != std::string::npos) {
            typeName.erase(0, qualifier + 2);
        }
    }
    std::string example;
    if (type.kind == TypeRef::Kind::Named) {
        example = std::format("{} = {} {{ ... }}", symbol->name, typeName);
    }
    else if (type.kind == TypeRef::Kind::Tuple) {
        example = std::format("{} = (...)", symbol->name);
    }
    else if (type.kind == TypeRef::Kind::Array) {
        example = std::format("{} = [...]", symbol->name);
    }
    const std::string help = example.empty()
                               ? std::format("initialize '{}' whole before writing its parts", symbol->name)
                               : std::format("initialize '{}' whole, as in '{}'", symbol->name, example);
    const bool needsDestruction = ClassifyTypeProperties(type).droppable;
    EmitError(location, message,
              {note, std::format("'{}' {}, and a value written into a part of storage that holds none would never be "
                                 "destroyed",
                                 typeName, needsDestruction ? "needs destruction" : "may need destruction")},
              help);
}

std::optional<MoveStateTracker::Issue> AnalysisContext::MoveTrackedExpression(const Expr &expression,
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

bool AnalysisContext::ValidateMoveSource(const Expr &expression, const SourceLocation location) {
    if (!CheckBorrowedMove(expression, location)) {
        return false;
    }
    const auto usesReferenceStorage = [&](this auto &&self, const Expr &candidate) -> bool {
        const Expr *object = nullptr;
        if (const auto *field = dynamic_cast<const FieldExpr *>(&candidate)) {
            object = field->object.get();
        }
        else if (const auto *index = dynamic_cast<const IndexExpr *>(&candidate);
                 index && !IsIndexOperatorCall(*index)) {
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
        else if (const auto *index = dynamic_cast<const IndexExpr *>(&candidate);
                 index && !IsIndexOperatorCall(*index)) {
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

bool AnalysisContext::RejectSelfMove(const Expr &target, const Expr &value, const SourceLocation location) {
    if (!SameStoragePlace(target, value)) {
        return false;
    }
    const std::string place = AnalyzeMovePlace(target).Display();
    EmitError(location, std::format("cannot move '{}' into itself", place),
              {"the assignment source and destination identify the same move-only storage"},
              "remove the assignment or assign a distinct value");
    return true;
}

bool AnalysisContext::RejectImplicitMove(const Expr &expression, const TypeRef &type, const ValueConsumptionKind kind,
                                         const SourceLocation location) {
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

void AnalysisContext::ConsumeValue(const Expr &expression, const TypeRef &type, const ValueConsumptionKind kind,
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
        if (IsStoredAggregate(type) && (place.IsNamedStorage() || place.IsBorrowedStorage())) {
            const FuncDecl *custom = nullptr;
            if (properties.copyOperation == TypeProperties::SpecialOperationState::Custom) {
                custom = LookupSourceSpecialOperation(type, "=", location);
                if (!custom) {
                    return;
                }
            }
            valueCopies.insert_or_assign(&expression, ValueCopy{kind, type, custom, location});
        }
        return;
    }
    if (kind == ValueConsumptionKind::ArrayRepeat && properties.IsMoveOnly()) {
        EmitError(location, std::format("array repeat element type '{}' must be copyable", type.ToString()),
                  {"'[value; count]' evaluates 'value' once and copies it into every element"},
                  "write the elements explicitly or initialize move-only elements in a loop");
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

bool AnalysisContext::IsFreshTemporarySource(const Expr &expression, const TypeRef &type,
                                             const TypeRef &targetType) const {
    if (type.IsUnknown() || type.kind == TypeRef::Kind::Reference || !(type == targetType)) {
        return false;
    }
    // `<-value` records its own transfer, which leaves nothing for a copy to read afterwards.
    if (dynamic_cast<const MoveExpr *>(&expression)) {
        return true;
    }
    const MovePlace place = AnalyzeMovePlace(expression);
    return !place.IsNamedStorage() && !place.IsBorrowedStorage();
}

void AnalysisContext::ConsumeExplicitValue(const Expr &expression, const TypeRef &type, const SourceLocation location) {
    if (!trackedFlowReachable || type.IsUnknown()) {
        return;
    }
    if (RejectSubsetViewUse(expression, location, "be moved")) {
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
            custom = LookupSourceSpecialOperation(type, "<-", location);
            if (!custom) {
                return;
            }
        }
        valueConsumptions.insert_or_assign(&expression, ValueConsumption{ValueConsumptionKind::ExplicitMove, type,
                                                                         location, custom, constructsDestination});
    }
}

void AnalysisContext::ConsumeRecordedValue(const Expr &expression, const ValueConsumptionKind kind,
                                           const SourceLocation location) {
    const auto type = expressionTypes.find(&expression);
    if (type != expressionTypes.end()) {
        ConsumeValue(expression, type->second, kind, location);
    }
}

std::vector<TypeRef> AnalysisContext::CheckCallArgumentValues(const CallExpr &call) {
    std::vector<TypeRef> types;
    types.reserve(call.args.size());
    for (const auto &argument : call.args) {
        const auto recorded = expressionTypes.find(argument.get());
        const TypeRef type = recorded == expressionTypes.end() ? CheckExpr(*argument) : recorded->second;
        types.push_back(type);
    }
    return types;
}

void AnalysisContext::ConsumeCallArguments(const CallExpr &call, const std::vector<TypeRef> &argumentTypes,
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

void AnalysisContext::ConsumeMethodReceiver(const CallExpr &call, const Expr &receiver, const TypeRef &receiverType,
                                            const FuncDecl &method) {
    const std::optional<TypeRef> declared = ResolveMethodReceiverType(receiverType, method);
    if (declared && declared->kind == TypeRef::Kind::Reference) {
        BeginReceiverReferenceBorrow(call, receiver, *declared);
        return;
    }
    if (!declared || declared->kind == TypeRef::Kind::Pointer || declared->IsSlice()) {
        return;
    }
    ConsumeValue(receiver, receiverType, ValueConsumptionKind::Receiver, call.location);
}
} // namespace Rux::SemanticDetail
