// Lowering for typed throws. A throw site is lowered as `?` lowers a propagation -- a match on the outcome its call
// returns -- except that the failure arm sends the error where analysis recorded it goes. One leaving the function is
// an ordinary return of the outer failure. One going to a handler is stored in the handler's outcome slot, after which
// a labeled `break` leaves the hidden loop the handled code runs in: the defers and locals of every scope in between
// are run and destroyed on the way out, exactly as for any `break`, and the handler's arms then match the slot. HIR,
// LIR and the back ends see only matches, loops and breaks they already lower.

#include "Lowering/AstToHir/Detail/AstToHirContext.h"
#include "Types/NativeLayout.h"

#include <format>
#include <utility>

namespace Rux::AstToHirDetail {
namespace {
HirExprPtr Variable(const std::string &name, const TypeRef &type, const SourceLocation location) {
    auto variable = std::make_unique<HirVarExpr>();
    variable->location = location;
    variable->name = name;
    variable->type = type;
    return variable;
}

/// A hidden local holding nothing until it is first stored; it has no drop flag, so whoever reads it takes the value.
HirStmtPtr Slot(const std::string &name, const TypeRef &type, const SourceLocation location) {
    auto local = std::make_unique<HirLetStmt>();
    local->location = location;
    local->isMut = true;
    local->name = name;
    local->type = type;
    return local;
}

HirStmtPtr Store(const std::string &name, const TypeRef &type, HirExprPtr value, const SourceLocation location) {
    auto store = std::make_unique<HirAssignExpr>();
    store->location = location;
    store->op = TokenKind::Assign;
    store->target = Variable(name, type, location);
    store->value = std::move(value);
    store->type = type;
    auto statement = std::make_unique<HirExprStmt>();
    statement->location = location;
    statement->expr = std::move(store);
    return statement;
}

HirStmtPtr Scope(HirBlock block, const SourceLocation location) {
    auto scope = std::make_unique<HirScopeStmt>();
    scope->location = location;
    scope->block = std::move(block);
    return scope;
}

HirStmtPtr Leave(const std::string &label, const SourceLocation location) {
    auto leave = std::make_unique<HirBreakStmt>();
    leave->location = location;
    leave->label = label;
    return leave;
}

std::unique_ptr<HirBindingPattern> Binding(const std::string &name, const TypeRef &type,
                                           const SourceLocation location) {
    auto bound = std::make_unique<HirBindingPattern>();
    bound->location = location;
    bound->name = name;
    bound->type = type;
    return bound;
}

/// The slot read as the subject of the match its arms make, which owns what it holds.
HirExprPtr SlotSubject(const std::string &slot, const TypeRef &outcomeType, const SourceLocation location) {
    HirExprPtr subject = Variable(slot, outcomeType, location);
    subject->consumption = ValueConsumptionKind::MatchSubject;
    return subject;
}
} // namespace

HirExprPtr AstToHirContext::LowerThrowSite(const Expr &expression, const ResolvedThrowSite &site) {
    const SourceLocation location = expression.location;
    const Expr *savedRaw = std::exchange(rawThrowSite, &expression);
    HirExprPtr outcome = LowerExpr(expression);
    rawThrowSite = savedRaw;
    const TypeRef outcomeType = outcome->type;
    if (!outcomeType.IsFallible()) {
        return outcome;
    }
    // The outcome is a temporary the match takes apart; what the source does with the call's value applies to the
    // success the match produces.
    outcome->consumption = ValueConsumptionKind::PropagationOperand;
    outcome->consumedBindingId = 0;
    const TypeRef &success = outcomeType.FallibleSuccess();
    const TypeRef &error = outcomeType.FallibleError();
    const std::size_t ordinal = throwOrdinal++;
    const std::string payloadName = std::format("$throw.value.{}", ordinal);
    const std::string errorName = std::format("$throw.error.{}", ordinal);

    HirMatchArm continuing;
    continuing.location = location;
    continuing.pattern =
        NativeCasePattern(outcomeType, NativeSuccessTag, success, Binding(payloadName, success, location), location);
    continuing.body = TransferredBinding(payloadName, success, location);

    auto thrown = std::make_unique<HirBlockExpr>();
    thrown->location = location;
    thrown->type = success;
    thrown->block.location = location;
    thrown->block.stmts.push_back(LowerThrowTo(site, TransferredBinding(errorName, error, location), location));
    HirMatchArm failing;
    failing.location = location;
    failing.pattern =
        NativeCasePattern(outcomeType, NativeFailureTag, error, Binding(errorName, error, location), location);
    failing.body = std::move(thrown);

    auto lowered = std::make_unique<HirMatchExpr>();
    lowered->location = location;
    lowered->type = success;
    lowered->subject = std::move(outcome);
    lowered->arms.push_back(std::move(continuing));
    lowered->arms.push_back(std::move(failing));
    return lowered;
}

HirStmtPtr AstToHirContext::LowerThrowTo(const ResolvedThrowSite &site, HirExprPtr error,
                                         const SourceLocation location) {
    const void *handler = site.handlerBlock ? static_cast<const void *>(site.handlerBlock)
                                            : static_cast<const void *>(site.handlerExpression);
    const auto active = handler ? activeThrowHandlers.find(handler) : activeThrowHandlers.end();
    if (active == activeThrowHandlers.end()) {
        if (handler) {
            diagnostics->push_back({Diagnostic::Severity::Error,
                                    currentFile,
                                    location,
                                    "cannot lower a throw to a handler that is not being lowered",
                                    {"semantic analysis sent an error to a handler outside the code being lowered"},
                                    "please report this compiler limitation with a minimal source example",
                                    {}});
        }
        // The error leaves through the function's outer failure channel, injected or widened into it.
        const TypeRef channel = currentReturnType.IsFallible() ? currentReturnType.FallibleError() : error->type;
        HirExprPtr outgoing = ConvertNative(std::move(error), channel, location);
        return LowerFunctionReturn(MakeNativeCase(currentReturnType, NativeFailureTag, std::move(outgoing), location),
                                   location);
    }
    const ActiveThrowHandler &target = active->second;
    HirBlock thrown;
    thrown.location = location;
    HirExprPtr stored = ConvertNative(std::move(error), target.outcomeType.FallibleError(), location);
    thrown.stmts.push_back(Store(target.slot, target.outcomeType,
                                 MakeNativeCase(target.outcomeType, NativeFailureTag, std::move(stored), location),
                                 location));
    auto leave = std::make_unique<HirBreakStmt>();
    leave->location = location;
    leave->label = target.label;
    leave->cleanups = cleanupPlanner.LoopExitActions(target.label);
    thrown.stmts.push_back(LowerLoopExit(std::move(leave), target.label, location));
    return Scope(std::move(thrown), location);
}

std::unique_ptr<HirLoopStmt> AstToHirContext::LowerHandledCode(const void *handler, const ActiveThrowHandler &active,
                                                               const std::function<void(HirBlock &)> &lower,
                                                               const SourceLocation location) {
    auto loop = std::make_unique<HirLoopStmt>();
    loop->location = location;
    loop->label = active.label;
    loop->body.location = location;
    const CleanupPlanner::LoopToken token = BeginLoop(loop->label, true);
    activeThrowHandlers.insert_or_assign(handler, active);
    lower(loop->body);
    activeThrowHandlers.erase(handler);
    loop->body.stmts.push_back(Leave(loop->label, location));
    EndLoop(token);
    return loop;
}

void AstToHirContext::LowerHandlerArms(const std::vector<MatchExpr::Arm> &arms, const TypeRef &outcome,
                                       const std::optional<TypeRef> &result, std::vector<HirMatchArm> &lowered) {
    const TypeRef &error = outcome.FallibleError();
    for (const auto &arm : arms) {
        HirMatchArm recovery;
        recovery.location = arm.location;
        PushScope();
        // The slot is the handler's own, so every arm owns the error it binds and destroys what it leaves.
        const bool savedOwnership = std::exchange(patternBindingsOwnPayload, true);
        const auto *guarded = AstCast<const GuardedPattern>(arm.pattern.get());
        const Pattern &errorPattern = guarded && guarded->inner ? *guarded->inner : *arm.pattern;
        HirPatternPtr inner = LowerPattern(errorPattern, error);
        HirPatternPtr failure = NativeCasePattern(outcome, NativeFailureTag, error, std::move(inner), arm.location);
        if (guarded) {
            auto withGuard = std::make_unique<HirGuardedPattern>();
            withGuard->location = guarded->location;
            withGuard->inner = std::move(failure);
            withGuard->guard = LowerExpr(*guarded->guard);
            failure = std::move(withGuard);
        }
        recovery.pattern = std::move(failure);
        patternBindingsOwnPayload = savedOwnership;
        const bool leaves = AstCast<const DivergeExpr>(arm.body.get()) || AstCast<const BlockExpr>(arm.body.get());
        recovery.body = !result || leaves ? LowerExpr(*arm.body) : LowerExprAs(*arm.body, *result);
        recovery.cleanups = CurrentScopeCleanups();
        PopScope();
        lowered.push_back(std::move(recovery));
    }
}

HirStmtPtr AstToHirContext::LowerTryStatement(const TryStmt &statement) {
    const SourceLocation location = statement.location;
    const ResolvedThrowHandler *handler = model.TryGetThrowHandler(statement);
    const TypeRef error = handler ? SubstituteCurrentType(handler->errorType) : TypeRef::MakeUnknown();
    // A block nothing throws in is only its statements.
    if (error.IsUnknown() || !statement.body) {
        HirBlock body = statement.body ? LowerBlock(*statement.body) : HirBlock{};
        body.location = location;
        return Scope(std::move(body), location);
    }
    const std::size_t ordinal = throwOrdinal++;
    ActiveThrowHandler active;
    active.label = std::format("$try.{}", ordinal);
    active.slot = std::format("$try.outcome.{}", ordinal);
    active.outcomeType = TypeRef::MakeFallible(TypeRef::MakeUnit(), error);

    HirBlock lowered;
    lowered.location = location;
    lowered.stmts.push_back(Slot(active.slot, active.outcomeType, location));
    lowered.stmts.push_back(LowerHandledCode(
        &statement, active,
        [&](HirBlock &code) {
            code.stmts.push_back(Scope(LowerBlock(*statement.body), statement.body->location));
            code.stmts.push_back(Store(
                active.slot, active.outcomeType,
                MakeNativeCase(active.outcomeType, NativeSuccessTag, MakeUnitValue(location), location), location));
        },
        location));

    auto arms = std::make_unique<HirMatchStmt>();
    arms->location = statement.catchLocation;
    arms->subject = SlotSubject(active.slot, active.outcomeType, location);
    HirMatchArm completed;
    completed.location = location;
    auto ignored = std::make_unique<HirWildcardPattern>();
    ignored->location = location;
    completed.pattern =
        NativeCasePattern(active.outcomeType, NativeSuccessTag, TypeRef::MakeUnit(), std::move(ignored), location);
    auto nothing = std::make_unique<HirBlockExpr>();
    nothing->location = location;
    nothing->type = TypeRef::MakeOpaque();
    nothing->block.location = location;
    completed.body = std::move(nothing);
    arms->arms.push_back(std::move(completed));
    LowerHandlerArms(statement.arms, active.outcomeType, std::nullopt, arms->arms);
    lowered.stmts.push_back(std::move(arms));
    return Scope(std::move(lowered), location);
}

HirExprPtr AstToHirContext::LowerTryMark(const TryMarkExpr &expression) {
    const SourceLocation location = expression.location;
    // A plain `try` only marks: each site it covers is lowered where it is.
    const ResolvedThrowHandler *handler =
        expression.form == TryMarkExpr::Form::Propagate ? nullptr : model.TryGetThrowHandler(expression);
    if (!handler) {
        return LowerExpr(*expression.operand);
    }
    const TypeRef value = SubstituteCurrentType(handler->valueType);
    const TypeRef error = SubstituteCurrentType(handler->errorType);
    const bool optional = expression.form == TryMarkExpr::Form::Optional;
    const TypeRef result = optional ? TypeRef::MakeOptional(value) : value;
    // Nothing throws: the operand is the value, made present for `try?`.
    if (error.IsUnknown()) {
        HirExprPtr lowered = LowerExprAs(*expression.operand, value);
        return optional ? MakeNativeCase(result, NativePresentTag, std::move(lowered), location) : std::move(lowered);
    }
    const std::size_t ordinal = throwOrdinal++;
    ActiveThrowHandler active;
    active.label = std::format("$try.{}", ordinal);
    active.slot = std::format("$try.outcome.{}", ordinal);
    active.outcomeType = TypeRef::MakeFallible(value, error);

    auto lowered = std::make_unique<HirBlockExpr>();
    lowered->location = location;
    lowered->type = result;
    lowered->block.location = location;
    lowered->block.stmts.push_back(Slot(active.slot, active.outcomeType, location));
    lowered->block.stmts.push_back(LowerHandledCode(
        &expression, active,
        [&](HirBlock &code) {
            code.stmts.push_back(Store(
                active.slot, active.outcomeType,
                MakeNativeCase(active.outcomeType, NativeSuccessTag, LowerExprAs(*expression.operand, value), location),
                location));
        },
        location));

    auto match = std::make_unique<HirMatchExpr>();
    match->location = location;
    match->type = result;
    match->subject = SlotSubject(active.slot, active.outcomeType, location);
    const std::string payloadName = std::format("$try.value.{}", ordinal);
    HirMatchArm succeeded;
    succeeded.location = location;
    succeeded.pattern =
        NativeCasePattern(active.outcomeType, NativeSuccessTag, value, Binding(payloadName, value, location), location);
    HirExprPtr payload = TransferredBinding(payloadName, value, location);
    succeeded.body =
        optional ? MakeNativeCase(result, NativePresentTag, std::move(payload), location) : std::move(payload);
    match->arms.push_back(std::move(succeeded));
    if (optional) {
        // The error is dropped with the arm that ignores it, and the result is absent.
        HirMatchArm failed;
        failed.location = location;
        auto ignored = std::make_unique<HirWildcardPattern>();
        ignored->location = location;
        failed.pattern = NativeCasePattern(active.outcomeType, NativeFailureTag, error, std::move(ignored), location);
        failed.body = MakeNativeCase(result, NativeAbsentTag, nullptr, location);
        match->arms.push_back(std::move(failed));
    }
    else {
        LowerHandlerArms(expression.arms, active.outcomeType, value, match->arms);
    }
    lowered->value = std::move(match);
    return lowered;
}
} // namespace Rux::AstToHirDetail
