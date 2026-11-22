// Lowering for errors. The operand of a postfix error form is a throw site, lowered as `?` lowers a propagation -- a
// match on the outcome the operand produces. For `x!` the failure arm is an ordinary return of the function's outer
// failure, so the defers and locals of every scope in between run and are destroyed as for any return. For `x catch
// { arms }`, `x !! v` and `x !!;` the failure arms are the handler's own. HIR, LIR and the back ends see only matches
// and returns they already lower.

#include "Lowering/AstToHir/Detail/AstToHirContext.h"
#include "Types/NativeLayout.h"

#include <format>
#include <utility>

namespace Rux::AstToHirDetail {
namespace {
std::unique_ptr<HirBindingPattern> Binding(const std::string &name, const TypeRef &type,
                                           const SourceLocation location) {
    auto bound = std::make_unique<HirBindingPattern>();
    bound->location = location;
    bound->name = name;
    bound->type = type;
    return bound;
}

HirExprPtr Nothing(const SourceLocation location) {
    auto empty = std::make_unique<HirBlockExpr>();
    empty->location = location;
    empty->type = TypeRef::MakeOpaque();
    empty->block.location = location;
    return empty;
}
} // namespace

HirExprPtr AstToHirContext::LowerSiteOutcome(const Expr &expression, const ResolvedThrowSite &site) {
    const Expr *savedRaw = std::exchange(rawThrowSite, &expression);
    HirExprPtr outcome = LowerExpr(expression);
    rawThrowSite = savedRaw;
    // A fallible kept in a local is recorded with its success type, as the expression reads where it is handled; the
    // read itself takes the whole outcome.
    if (!outcome->type.IsFallible() && !AstCast<const CallExpr>(&expression)) {
        outcome->type =
            TypeRef::MakeFallible(SubstituteCurrentType(site.payloadType), SubstituteCurrentType(site.errorType));
    }
    if (outcome->type.IsFallible()) {
        // The outcome is a temporary the match takes apart; what the source does with the value applies to the
        // success the match produces.
        outcome->consumption = ValueConsumptionKind::PropagationOperand;
        outcome->consumedBindingId = 0;
    }
    return outcome;
}

HirExprPtr AstToHirContext::LowerThrowSite(const Expr &expression, const ResolvedThrowSite &site) {
    const SourceLocation location = expression.location;
    HirExprPtr outcome = LowerSiteOutcome(expression, site);
    const TypeRef outcomeType = outcome->type;
    if (!outcomeType.IsFallible()) {
        return outcome;
    }
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
    thrown->block.stmts.push_back(LowerThrowTo(TransferredBinding(errorName, error, location), location));
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

HirStmtPtr AstToHirContext::LowerThrowTo(HirExprPtr error, const SourceLocation location) {
    // The error leaves through the function's outer failure channel, injected or widened into it.
    const TypeRef channel = currentReturnType.IsFallible() ? currentReturnType.FallibleError() : error->type;
    HirExprPtr outgoing = ConvertNative(std::move(error), channel, location);
    return LowerFunctionReturn(MakeNativeCase(currentReturnType, NativeFailureTag, std::move(outgoing), location),
                               location);
}

void AstToHirContext::LowerHandlerArms(const std::vector<MatchExpr::Arm> &arms, const TypeRef &outcome,
                                       const std::optional<TypeRef> &result, std::vector<HirMatchArm> &lowered) {
    const TypeRef &error = outcome.FallibleError();
    for (const auto &arm : arms) {
        HirMatchArm recovery;
        recovery.location = arm.location;
        PushScope();
        // The outcome is the handler's own, so every arm owns the error it binds and destroys what it leaves.
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

HirExprPtr AstToHirContext::LowerErrorForm(const ErrorFormExpr &expression) {
    const SourceLocation location = expression.location;
    // `x!` is lowered where its operand is, as the throw site that leaves the function.
    const ResolvedThrowHandler *handler =
        expression.form == ErrorFormExpr::Form::Propagate ? nullptr : model.TryGetThrowHandler(expression);
    if (!handler) {
        return LowerExpr(*expression.operand);
    }
    const TypeRef value = SubstituteCurrentType(handler->valueType);
    const TypeRef error = SubstituteCurrentType(handler->errorType);
    // A handler written as a whole statement drops the success, and `x !! none` may produce what its context expects.
    const bool discards = handler->discardsValue;
    const TypeRef result = discards                         ? TypeRef::MakeOpaque()
                         : !handler->resultType.IsUnknown() ? SubstituteCurrentType(handler->resultType)
                                                            : value;
    const ResolvedThrowSite *site = model.TryGetThrowSite(*expression.operand);
    if (error.IsUnknown() || !site) {
        return LowerExprAs(*expression.operand, value);
    }
    HirExprPtr outcome = LowerSiteOutcome(*expression.operand, *site);
    const TypeRef outcomeType = outcome->type.IsFallible() ? outcome->type : TypeRef::MakeFallible(value, error);
    const std::size_t ordinal = throwOrdinal++;

    auto match = std::make_unique<HirMatchExpr>();
    match->location = location;
    match->type = result;
    match->subject = std::move(outcome);
    const std::string payloadName = std::format("$catch.value.{}", ordinal);
    // A wildcard that destroys what it matches: the part of the outcome a discarding handler drops.
    const auto discarded = [&](const TypeRef &type) {
        auto wildcard = std::make_unique<HirWildcardPattern>();
        wildcard->location = location;
        if (const DropGluePlan *glue = model.TryGetDropGlue(type)) {
            wildcard->discardGlue = glue->symbol;
        }
        return wildcard;
    };
    HirMatchArm succeeded;
    succeeded.location = location;
    if (discards) {
        // The success nobody takes is destroyed where it is matched, as a temporary nobody kept.
        succeeded.pattern = NativeCasePattern(outcomeType, NativeSuccessTag, value, discarded(value), location);
        succeeded.body = Nothing(location);
    }
    else {
        succeeded.pattern =
            NativeCasePattern(outcomeType, NativeSuccessTag, value, Binding(payloadName, value, location), location);
        succeeded.body = ConvertNative(TransferredBinding(payloadName, value, location), result, location);
    }
    match->arms.push_back(std::move(succeeded));
    if (expression.ignoresError) {
        // `x !!;` drops the error with the arm that ignores it.
        HirMatchArm failed;
        failed.location = location;
        failed.pattern = NativeCasePattern(outcomeType, NativeFailureTag, error, discarded(error), location);
        failed.body = Nothing(location);
        match->arms.push_back(std::move(failed));
    }
    else {
        LowerHandlerArms(expression.arms, outcomeType, discards ? std::nullopt : std::optional<TypeRef>(result),
                         match->arms);
    }
    return match;
}
} // namespace Rux::AstToHirDetail
