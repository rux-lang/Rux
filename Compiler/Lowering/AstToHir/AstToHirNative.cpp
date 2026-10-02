// Native construction lowering: `.Success`, `.Failure`, `.Some`, `none`, the unit value, and every implicit conversion
// into a native type become explicit tagged constructions, so later stages see one form for building a native level.
//
// A native level is built as a variant with one single-field case per alternative: the tag is the case's native tag
// and the payload is the alternative's value. A conversion follows the route the conversion rules select for the
// substituted source and destination types: constructing presence or success wraps the value, injecting a member
// selects that member's tag, and widening an existing value is a match over its alternatives that rebuilds each one
// under the destination's tags, so no stage ever reinterprets a tag in place.

#include "Lowering/AstToHir/Detail/AstToHirContext.h"
#include "Types/NativeConversion.h"
#include "Types/NativeLayout.h"

#include <algorithm>
#include <format>
#include <utility>

namespace Rux::AstToHirDetail {
namespace {
[[nodiscard]] std::string NativeBindingName(const std::size_t ordinal) {
    return std::format("$native.payload.{}", ordinal);
}

/// The index of `member` among the members of the sum `sum`.
[[nodiscard]] std::optional<std::uint64_t> MemberTag(const TypeRef &sum, const TypeRef &member) {
    const auto found = std::ranges::find(sum.inner, member);
    if (found == sum.inner.end()) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(found - sum.inner.begin());
}

/// The alternatives of one native level, in tag order, with the payload each carries.
[[nodiscard]] std::vector<std::pair<std::uint64_t, std::optional<TypeRef>>> NativeCases(const TypeRef &type) {
    std::vector<std::pair<std::uint64_t, std::optional<TypeRef>>> cases;
    if (type.IsSum()) {
        for (std::size_t index = 0; index < type.inner.size(); ++index) {
            cases.emplace_back(index, type.inner[index]);
        }
    }
    else if (type.IsOptional()) {
        cases.emplace_back(NativeAbsentTag, std::nullopt);
        cases.emplace_back(NativePresentTag, type.inner.front());
    }
    else if (type.IsFallible()) {
        cases.emplace_back(NativeSuccessTag, type.FallibleSuccess());
        cases.emplace_back(NativeFailureTag, type.FallibleError());
    }
    return cases;
}
} // namespace

bool AstToHirContext::IsNativeType(const TypeRef &type) {
    return type.IsSum() || type.IsOptional() || type.IsFallible();
}

bool AstToHirContext::CompletesWithoutValue(const TypeRef &returnType) {
    return returnType.IsUnit() || (returnType.IsFallible() && returnType.FallibleSuccess().IsUnit());
}

HirExprPtr AstToHirContext::MakeUnitValue(const SourceLocation location) {
    auto unit = std::make_unique<HirTupleExpr>();
    unit->location = location;
    unit->type = TypeRef::MakeUnit();
    return unit;
}

HirExprPtr AstToHirContext::CompletionValue(const TypeRef &returnType, const SourceLocation location) {
    if (returnType.IsFallible()) {
        return MakeNativeCase(returnType, NativeSuccessTag, MakeUnitValue(location), location);
    }
    return MakeUnitValue(location);
}

HirExprPtr AstToHirContext::MakeNativeCase(const TypeRef &type, const std::uint64_t tag, HirExprPtr payload,
                                           const SourceLocation location) {
    auto construct = std::make_unique<HirEnumConstructExpr>();
    construct->location = location;
    construct->form = CaseTypeForm::Variant;
    construct->type = type;
    construct->discriminant = std::to_string(tag);
    if (payload) {
        AppendFailureCleanup(construct->failureCleanups, {});
        construct->payloads.push_back(std::move(payload));
    }
    return construct;
}

std::unique_ptr<HirEnumPattern> AstToHirContext::NativeCasePattern(const TypeRef &type, const std::uint64_t tag,
                                                                   const std::optional<TypeRef> &payloadType,
                                                                   HirPatternPtr payload,
                                                                   const SourceLocation location) {
    auto pattern = std::make_unique<HirEnumPattern>();
    pattern->location = location;
    pattern->resolvedType = type;
    pattern->form = CaseTypeForm::Variant;
    pattern->discriminant = std::to_string(tag);
    pattern->hasPayload = payloadType.has_value();
    if (payloadType) {
        pattern->payloadTypes.push_back(*payloadType);
        if (payload) {
            pattern->argIndices.push_back(0);
            pattern->args.push_back(std::move(payload));
        }
    }
    return pattern;
}

HirExprPtr AstToHirContext::TransferredBinding(const std::string &name, const TypeRef &type,
                                               const SourceLocation location) {
    auto value = std::make_unique<HirVarExpr>();
    value->location = location;
    value->name = name;
    value->type = type;
    HirMovePlan plan = BuildMovePlan(type);
    if (plan.kind == HirMovePlan::Kind::Trivial) {
        return value;
    }
    auto moved = std::make_unique<HirMoveExpr>();
    moved->location = location;
    moved->type = type;
    moved->plan = std::move(plan);
    moved->value = std::move(value);
    return moved;
}

HirExprPtr AstToHirContext::RebuildNativeLevel(
    HirExprPtr value, const TypeRef &destination,
    const std::function<HirExprPtr(HirExprPtr, const TypeRef &, const TypeRef &)> &convertPayload,
    const SourceLocation location) {
    // Every alternative of the source keeps its meaning under the destination's tags: the match takes each payload
    // out of the evaluated source and builds the destination case that alternative belongs to.
    const TypeRef source = value->type;
    auto match = std::make_unique<HirMatchExpr>();
    match->location = location;
    match->type = destination;
    for (const auto &[tag, payloadType] : NativeCases(source)) {
        HirMatchArm arm;
        arm.location = location;
        const std::string name = NativeBindingName(nativeOrdinal++);
        HirPatternPtr binding;
        if (payloadType) {
            auto bound = std::make_unique<HirBindingPattern>();
            bound->location = location;
            bound->name = name;
            bound->type = *payloadType;
            binding = std::move(bound);
        }
        arm.pattern = NativeCasePattern(source, tag, payloadType, std::move(binding), location);
        std::uint64_t destinationTag = tag;
        std::optional<TypeRef> destinationPayload;
        if (destination.IsSum()) {
            destinationTag = MemberTag(destination, *payloadType).value_or(0);
            destinationPayload = *payloadType;
        }
        else if (payloadType) {
            destinationPayload = destination.inner[tag == NativeFailureTag && destination.IsFallible() ? 1 : 0];
        }
        HirExprPtr payload = payloadType ? TransferredBinding(name, *payloadType, location) : nullptr;
        if (payload && destinationPayload && *destinationPayload != *payloadType) {
            payload = convertPayload(std::move(payload), *payloadType, *destinationPayload);
        }
        arm.body = MakeNativeCase(destination, destinationTag, std::move(payload), location);
        match->arms.push_back(std::move(arm));
    }
    match->subject = std::move(value);
    return match;
}

HirExprPtr AstToHirContext::ConvertNative(HirExprPtr value, const TypeRef &destination, const SourceLocation location) {
    const TypeRef source = value->type;
    // A value with no type of its own -- a block, or a diverging body -- never reaches a destination to convert to.
    if (source == destination || !IsNativeType(destination) || source.IsUnknown() || source.IsOpaque()) {
        return value;
    }
    const NativeConversion conversion = ClassifyNativeConversion(source, destination);
    if (!conversion.Accepted()) {
        diagnostics.push_back(
            {Diagnostic::Severity::Error,
             currentFile,
             location,
             std::format("cannot lower the conversion of '{}' to '{}'", source.ToString(), destination.ToString()),
             {"semantic analysis accepted a native conversion that has no route after substitution"},
             "please report this compiler limitation with a minimal source example",
             {}});
        return value;
    }
    if (conversion.outcome == NativeConversion::Outcome::Identity) {
        return value;
    }
    return ApplyNativeRoute(std::move(value), destination, conversion.route, location);
}

HirExprPtr AstToHirContext::ApplyNativeRoute(HirExprPtr value, const TypeRef &destination,
                                             const std::span<const NativeConversionStep> route,
                                             const SourceLocation location) {
    if (route.empty()) {
        return value;
    }
    const NativeConversionStep &step = route.front();
    const auto rest = route.subspan(1);
    const auto widenMember = [&](HirExprPtr payload, const TypeRef &, const TypeRef &target) {
        return ConvertNative(std::move(payload), target, location);
    };
    switch (step.kind) {
    case NativeConversionStep::Kind::Identity:
        return value;
    case NativeConversionStep::Kind::Inject:
        return MakeNativeCase(destination, step.member, std::move(value), location);
    case NativeConversionStep::Kind::Presence:
        return MakeNativeCase(destination, NativePresentTag,
                              ApplyNativeRoute(std::move(value), destination.inner.front(), rest, location), location);
    case NativeConversionStep::Kind::Success:
        return MakeNativeCase(destination, NativeSuccessTag,
                              ApplyNativeRoute(std::move(value), destination.inner.front(), rest, location), location);
    case NativeConversionStep::Kind::Absent:
        return MakeNativeCase(destination, NativeAbsentTag, nullptr, location);
    case NativeConversionStep::Kind::WidenSum:
    case NativeConversionStep::Kind::WidenOptional:
    case NativeConversionStep::Kind::WidenFallible:
        return RebuildNativeLevel(std::move(value), destination, widenMember, location);
    }
    return value;
}

HirExprPtr AstToHirContext::LowerNoneAs(const TypeRef &targetType, const SourceLocation location) {
    // `none` follows success levels until it reaches an optional, and is that optional's absence.
    if (targetType.IsFallible()) {
        return MakeNativeCase(targetType, NativeSuccessTag, LowerNoneAs(targetType.FallibleSuccess(), location),
                              location);
    }
    if (!targetType.IsOptional()) {
        diagnostics.push_back({Diagnostic::Severity::Error,
                               currentFile,
                               location,
                               std::format("cannot lower 'none' as '{}'", targetType.ToString()),
                               {"semantic analysis accepted 'none' without an optional destination"},
                               "please report this compiler limitation with a minimal source example",
                               {}});
    }
    return MakeNativeCase(targetType, NativeAbsentTag, nullptr, location);
}

HirExprPtr AstToHirContext::LowerNativeConstruct(const NativeConstructExpr &construct, const TypeRef &targetType) {
    // A constructor builds its own level, inside whatever presence or success wrappers its context adds around it.
    const bool optionalForm = construct.kind == NativeConstructExpr::Kind::Some;
    if (optionalForm ? targetType.IsFallible() : targetType.IsOptional()) {
        const std::uint64_t wrapperTag = optionalForm ? NativeSuccessTag : NativePresentTag;
        return MakeNativeCase(targetType, wrapperTag, LowerNativeConstruct(construct, targetType.inner.front()),
                              construct.location);
    }
    std::uint64_t tag = NativePresentTag;
    TypeRef channel = TypeRef::MakeUnknown();
    if (optionalForm && targetType.IsOptional()) {
        channel = targetType.inner.front();
    }
    else if (!optionalForm && targetType.IsFallible()) {
        const bool failure = construct.kind == NativeConstructExpr::Kind::Failure;
        tag = failure ? NativeFailureTag : NativeSuccessTag;
        channel = failure ? targetType.FallibleError() : targetType.FallibleSuccess();
    }
    HirExprPtr payload =
        construct.operand ? LowerExprAs(*construct.operand, channel) : MakeUnitValue(construct.location);
    return MakeNativeCase(targetType, tag, std::move(payload), construct.location);
}

HirExprPtr AstToHirContext::LowerNativeAs(const Expr &expression, const TypeRef &targetType) {
    if (dynamic_cast<const NativeConstructExpr *>(&expression) || dynamic_cast<const NoneExpr *>(&expression)) {
        // Lowered through the ordinary path so the constructor's own consumption facts still apply, with the
        // destination it is built as.
        const std::optional<TypeRef> saved = std::exchange(pendingNativeTarget, targetType);
        HirExprPtr lowered = LowerExpr(expression);
        pendingNativeTarget = saved;
        return lowered;
    }

    // An unsuffixed literal has no width of its own: it descends through presence and success levels and takes the one
    // member of its literal kind in the sum it reaches.
    const auto *literal = dynamic_cast<const LiteralExpr *>(&expression);
    const auto *negated = dynamic_cast<const UnaryExpr *>(&expression);
    if (!literal && negated && negated->op == TokenKind::Minus) {
        literal = dynamic_cast<const LiteralExpr *>(negated->operand.get());
    }
    const bool unsuffixedNumber =
        literal && (literal->token.kind == TokenKind::IntLiteral || literal->token.kind == TokenKind::FloatLiteral) &&
        NumericLiteralSuffix(literal->token.text).empty();
    if (unsuffixedNumber) {
        const bool floating = literal->token.kind == TokenKind::FloatLiteral;
        const std::function<HirExprPtr(const TypeRef &)> build = [&](const TypeRef &level) -> HirExprPtr {
            if (level.IsOptional() || level.IsFallible()) {
                return MakeNativeCase(level, level.IsOptional() ? NativePresentTag : NativeSuccessTag,
                                      build(level.inner.front()), expression.location);
            }
            if (level.IsSum()) {
                const auto member = std::ranges::find_if(level.inner, [&](const TypeRef &candidate) {
                    return floating ? candidate.IsFloat() : candidate.IsInteger();
                });
                if (member != level.inner.end()) {
                    return MakeNativeCase(level, static_cast<std::uint64_t>(member - level.inner.begin()),
                                          LowerExprAs(expression, *member), expression.location);
                }
            }
            return LowerExprAs(expression, level);
        };
        return build(targetType);
    }

    // A conditional chooses between two values that each convert on their own path, so a constructor or `none` in an
    // arm is built directly as the destination.
    if (const auto *ternary = dynamic_cast<const TernaryExpr *>(&expression)) {
        auto lowered = std::make_unique<HirTernaryExpr>();
        lowered->location = ternary->location;
        lowered->type = targetType;
        lowered->condition = LowerExpr(*ternary->condition);
        lowered->thenExpr = LowerExprAs(*ternary->thenExpr, targetType);
        lowered->elseExpr = LowerExprAs(*ternary->elseExpr, targetType);
        return lowered;
    }

    HirExprPtr lowered = LowerExpr(expression);
    return ConvertNative(std::move(lowered), targetType, expression.location);
}

HirStmtPtr AstToHirContext::LowerFail(const Expr *value, const SourceLocation location) {
    // `fail` selects the outer failure channel itself; only its operand converts, to that channel's payload.
    const TypeRef error = currentReturnType.IsFallible() ? currentReturnType.FallibleError() : TypeRef::MakeUnknown();
    HirExprPtr payload = value ? LowerExprAs(*value, error) : MakeUnitValue(location);
    return LowerFunctionReturn(MakeNativeCase(currentReturnType, NativeFailureTag, std::move(payload), location),
                               location);
}

HirExprPtr AstToHirContext::LowerDiverge(const DivergeExpr &expression) {
    HirStmtPtr statement;
    switch (expression.kind) {
    case DivergeExpr::Kind::Return: {
        HirExprPtr value = expression.value ? LowerExprAs(*expression.value, currentReturnType) : nullptr;
        if (!value && CompletesWithoutValue(currentReturnType)) {
            value = CompletionValue(currentReturnType, expression.location);
        }
        statement = LowerFunctionReturn(std::move(value), expression.location);
        break;
    }
    case DivergeExpr::Kind::Fail:
        statement = LowerFail(expression.value.get(), expression.location);
        break;
    case DivergeExpr::Kind::Break: {
        auto exit = std::make_unique<HirBreakStmt>();
        exit->location = expression.location;
        exit->label = expression.label;
        exit->cleanups = cleanupPlanner.LoopExitActions(expression.label);
        statement = std::move(exit);
        break;
    }
    case DivergeExpr::Kind::Continue: {
        auto exit = std::make_unique<HirContinueStmt>();
        exit->location = expression.location;
        exit->label = expression.label;
        exit->cleanups = cleanupPlanner.LoopExitActions(expression.label);
        statement = std::move(exit);
        break;
    }
    }
    auto block = std::make_unique<HirBlockExpr>();
    block->location = expression.location;
    block->type = TypeRef::MakeUnknown();
    block->block.location = expression.location;
    block->block.stmts.push_back(std::move(statement));
    return block;
}

HirExprPtr AstToHirContext::LowerPendingNative(const Expr &expression) {
    const TypeRef targetType = pendingNativeTarget ? *pendingNativeTarget : ResolvedExpressionType(expression);
    pendingNativeTarget.reset();
    if (const auto *construct = dynamic_cast<const NativeConstructExpr *>(&expression)) {
        return LowerNativeConstruct(*construct, targetType);
    }
    return LowerNoneAs(targetType, expression.location);
}
} // namespace Rux::AstToHirDetail
