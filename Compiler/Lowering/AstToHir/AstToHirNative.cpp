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
#include "Types/NativeSelection.h"

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

std::vector<std::pair<std::uint64_t, std::optional<TypeRef>>> AstToHirContext::NativeCaseList(const TypeRef &type) {
    return NativeCases(type);
}

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
    if (type.IsSum()) {
        pattern->path = {payloadType ? payloadType->ToString() : std::to_string(tag)};
    }
    else if (type.IsOptional()) {
        pattern->path = {tag == NativeAbsentTag ? "none" : ".Some"};
    }
    else {
        pattern->path = {tag == NativeFailureTag ? ".Failure" : ".Success"};
    }
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

HirCopyPlan AstToHirContext::BuildNativeCopyPlan(const TypeRef &type) {
    HirCopyPlan plan;
    plan.type = type;
    plan.form = CaseTypeForm::Variant;
    bool needsPlan = false;
    for (const auto &[tag, payloadType] : NativeCases(type)) {
        plan.variantDiscriminants.push_back(std::to_string(tag));
        std::vector<TypeRef> payloadTypes;
        std::vector<HirCopyPlan> payloadPlans;
        if (payloadType) {
            payloadTypes.push_back(*payloadType);
            payloadPlans.push_back(BuildCopyPlan(*payloadType));
            needsPlan = needsPlan || payloadPlans.back().kind != HirCopyPlan::Kind::Trivial;
        }
        plan.variantPayloadTypes.push_back(std::move(payloadTypes));
        plan.variantComponents.push_back(std::move(payloadPlans));
    }
    if (needsPlan) {
        plan.kind = HirCopyPlan::Kind::Enum;
    }
    else {
        plan.variantDiscriminants.clear();
        plan.variantPayloadTypes.clear();
        plan.variantComponents.clear();
    }
    return plan;
}

HirMovePlan AstToHirContext::BuildNativeMovePlan(const TypeRef &type) {
    HirMovePlan plan;
    plan.type = type;
    plan.form = CaseTypeForm::Variant;
    bool needsPlan = false;
    for (const auto &[tag, payloadType] : NativeCases(type)) {
        plan.variantDiscriminants.push_back(std::to_string(tag));
        std::vector<TypeRef> payloadTypes;
        std::vector<HirMovePlan> payloadPlans;
        if (payloadType) {
            payloadTypes.push_back(*payloadType);
            payloadPlans.push_back(BuildMovePlan(*payloadType));
            needsPlan = needsPlan || payloadPlans.back().kind != HirMovePlan::Kind::Trivial;
        }
        plan.variantPayloadTypes.push_back(std::move(payloadTypes));
        plan.variantComponents.push_back(std::move(payloadPlans));
    }
    if (needsPlan) {
        plan.kind = HirMovePlan::Kind::Variant;
    }
    else {
        plan.variantDiscriminants.clear();
        plan.variantPayloadTypes.clear();
        plan.variantComponents.clear();
    }
    return plan;
}

HirPatternPtr AstToHirContext::LowerPatternBinding(const std::string &name, const TypeRef &type, const Pattern *source,
                                                   const SourceLocation location) {
    auto lowered = std::make_unique<HirBindingPattern>();
    lowered->location = location;
    lowered->name = name;
    lowered->type = type;
    HirSymbol symbol;
    symbol.kind = HirSymbol::Kind::Var;
    symbol.name = name;
    symbol.type = type;
    if (patternBindingsOwnPayload) {
        symbol.bindingId = RegisterCleanupBinding(symbol.name, symbol.type, location);
    }
    lowered->bindingId = symbol.bindingId;
    const PatternBindingMode *mode = source ? model.TryGetPatternBindingMode(*source) : nullptr;
    lowered->alias = mode && *mode == PatternBindingMode::Alias;
    Define(std::move(symbol));
    return lowered;
}

HirPatternPtr AstToHirContext::LowerNativeSelection(const TypeRef &subjectType, const TypeRef &annotation,
                                                    const std::string &name, const Pattern *source,
                                                    const SourceLocation location) {
    const auto binding = [&](const TypeRef &type) -> HirPatternPtr {
        if (name.empty()) {
            return nullptr;
        }
        return LowerPatternBinding(name, type, source, location);
    };
    const auto members = [&](const TypeRef &sum, const std::vector<std::size_t> &selected) -> HirPatternPtr {
        if (selected.size() == 1) {
            const TypeRef &member = sum.inner[selected.front()];
            return NativeCasePattern(sum, selected.front(), member, binding(member), location);
        }
        auto subset = std::make_unique<HirNativeSubsetPattern>();
        subset->location = location;
        subset->subsetType = annotation.IsSum() ? annotation : sum;
        for (std::size_t index = 0; index < selected.size(); ++index) {
            subset->tags.emplace_back(std::to_string(selected[index]), std::to_string(index));
        }
        subset->binding = binding(subset->subsetType);
        return subset;
    };
    const NativeSelection selection = ClassifyNativeSelection(subjectType, annotation);
    switch (selection.kind) {
    case NativeSelection::Kind::Whole:
        if (HirPatternPtr whole = binding(subjectType)) {
            return whole;
        }
        break;
    case NativeSelection::Kind::Members:
        return members(subjectType, selection.members);
    case NativeSelection::Kind::Presence: {
        const TypeRef &payload = subjectType.inner.front();
        HirPatternPtr inner = selection.members.empty() ? binding(payload) : members(payload, selection.members);
        return NativeCasePattern(subjectType, NativePresentTag, payload, std::move(inner), location);
    }
    case NativeSelection::Kind::Invalid:
        diagnostics.push_back(
            {Diagnostic::Severity::Error,
             currentFile,
             location,
             std::format("cannot lower the selection of '{}' from '{}'", annotation.ToString(), subjectType.ToString()),
             {"semantic analysis accepted a typed pattern that selects nothing after substitution"},
             "please report this compiler limitation with a minimal source example",
             {}});
        break;
    }
    auto wildcard = std::make_unique<HirWildcardPattern>();
    wildcard->location = location;
    return wildcard;
}

HirPatternPtr AstToHirContext::LowerNativePattern(const Pattern &pattern, const TypeRef &subjectType) {
    const SourceLocation location = pattern.location;
    if (const auto *typed = dynamic_cast<const TypedPattern *>(&pattern)) {
        const TypeRef *recorded = model.TryGetTypedPatternType(*typed);
        const TypeRef annotation = recorded ? SubstituteCurrentType(*recorded) : subjectType;
        return LowerNativeSelection(subjectType, annotation, typed->name, &pattern, location);
    }
    if (dynamic_cast<const NonePattern *>(&pattern)) {
        return NativeCasePattern(subjectType, NativeAbsentTag, std::nullopt, nullptr, location);
    }
    if (const auto *presence = dynamic_cast<const PresencePattern *>(&pattern)) {
        const TypeRef payload = subjectType.IsOptional() ? subjectType.inner.front() : TypeRef::MakeUnknown();
        return NativeCasePattern(subjectType, NativePresentTag, payload, LowerPattern(*presence->inner, payload),
                                 location);
    }
    if (const auto *enumeration = dynamic_cast<const EnumPattern *>(&pattern);
        enumeration && enumeration->path.size() == 1 && enumeration->args.size() == 1 &&
        (subjectType.IsOptional() || subjectType.IsFallible())) {
        const std::string &name = enumeration->path.front();
        std::uint64_t tag = NativePresentTag;
        TypeRef channel = subjectType.inner.front();
        if (subjectType.IsFallible()) {
            const bool failure = name == "Failure";
            tag = failure ? NativeFailureTag : NativeSuccessTag;
            channel = failure ? subjectType.FallibleError() : subjectType.FallibleSuccess();
        }
        return NativeCasePattern(subjectType, tag, channel, LowerPattern(*enumeration->args.front(), channel),
                                 location);
    }
    if (subjectType.IsSum()) {
        if (const TypeRef *recorded = model.TryGetSumMember(pattern)) {
            const TypeRef member = SubstituteCurrentType(*recorded);
            if (const std::optional<std::uint64_t> tag = MemberTag(subjectType, member)) {
                return NativeCasePattern(subjectType, *tag, member, LowerPattern(pattern, member), location);
            }
        }
    }
    return nullptr;
}

HirExprPtr AstToHirContext::LowerNativeMembership(const IsExpr &expression) {
    TypeRef subjectType = ResolvedExpressionType(*expression.operand);
    if (subjectType.kind == TypeRef::Kind::Reference && !subjectType.inner.empty()) {
        subjectType = subjectType.inner.front();
    }
    if (!IsNativeType(subjectType)) {
        return nullptr;
    }
    const TypeRef *tested = model.TryGetType(*expression.type);
    const TypeRef testedType = tested ? SubstituteCurrentType(*tested) : ResolveType(*expression.type);
    const auto literal = [&](const bool value) {
        auto result = std::make_unique<HirLiteralExpr>();
        result->location = expression.location;
        result->type = TypeRef::MakeBool();
        result->value = value ? "true" : "false";
        return result;
    };

    // A test selects without binding, so the subject is inspected in place and never consumed.
    auto match = std::make_unique<HirMatchExpr>();
    match->location = expression.location;
    match->type = TypeRef::MakeBool();
    match->subject = LowerMatchSubject(*expression.operand);
    match->subject->consumption.reset();
    match->subject->consumedBindingId = 0;
    HirMatchArm selected;
    selected.location = expression.location;
    selected.pattern = LowerNativeSelection(subjectType, testedType, {}, nullptr, expression.location);
    selected.body = literal(true);
    HirMatchArm otherwise;
    otherwise.location = expression.location;
    auto wildcard = std::make_unique<HirWildcardPattern>();
    wildcard->location = expression.location;
    otherwise.pattern = std::move(wildcard);
    otherwise.body = literal(false);
    match->arms.push_back(std::move(selected));
    match->arms.push_back(std::move(otherwise));
    return match;
}

HirExprPtr AstToHirContext::LowerNativeTry(const TryExpr &expression) {
    const SourceLocation location = expression.location;
    const TypeRef operandType = ResolvedExpressionType(*expression.operand);
    if (operandType.IsOptional()) {
        return LowerNativeOptionalTry(expression, operandType);
    }
    const TypeRef &success = operandType.FallibleSuccess();
    const TypeRef &error = operandType.FallibleError();
    const TypeRef returnType = currentReturnType;
    const std::size_t ordinal = propagationOrdinal++;
    const std::string payloadName = std::format("$try.value.{}", ordinal);
    const std::string failureName = std::format("$try.failure.{}", ordinal);
    const auto binding = [&](const std::string &name, const TypeRef &type) {
        auto bound = std::make_unique<HirBindingPattern>();
        bound->location = location;
        bound->name = name;
        bound->type = type;
        return bound;
    };

    HirMatchArm continuing;
    continuing.location = location;
    continuing.pattern =
        NativeCasePattern(operandType, NativeSuccessTag, success, binding(payloadName, success), location);
    continuing.body = TransferredBinding(payloadName, success, location);

    // The failure leaves through the enclosing function's outer failure channel, chosen directly: the error is only
    // injected or widened into that channel, and the exit is an ordinary return with its defers and cleanup.
    HirExprPtr outgoing = ConvertNative(TransferredBinding(failureName, error, location),
                                        returnType.IsFallible() ? returnType.FallibleError() : error, location);
    auto exit = std::make_unique<HirBlockExpr>();
    exit->location = location;
    exit->type = success;
    exit->block.location = location;
    exit->block.stmts.push_back(
        LowerFunctionReturn(MakeNativeCase(returnType, NativeFailureTag, std::move(outgoing), location), location));
    HirMatchArm failing;
    failing.location = location;
    failing.pattern = NativeCasePattern(operandType, NativeFailureTag, error, binding(failureName, error), location);
    failing.body = std::move(exit);

    auto lowered = std::make_unique<HirMatchExpr>();
    lowered->location = location;
    lowered->type = success;
    lowered->subject = LowerExpr(*expression.operand);
    lowered->arms.push_back(std::move(continuing));
    lowered->arms.push_back(std::move(failing));
    return lowered;
}

HirExprPtr AstToHirContext::LowerMappedTry(const MappedTryExpr &expression) {
    const SourceLocation location = expression.location;
    const TypeRef operandType = ResolvedExpressionType(*expression.operand);
    const TypeRef &success = operandType.FallibleSuccess();
    const TypeRef &error = operandType.FallibleError();
    const TypeRef returnType = currentReturnType;
    const std::string payloadName = std::format("$try.value.{}", propagationOrdinal++);

    auto lowered = std::make_unique<HirMatchExpr>();
    lowered->location = location;
    lowered->type = success;
    lowered->subject = LowerExpr(*expression.operand);

    auto payload = std::make_unique<HirBindingPattern>();
    payload->location = location;
    payload->name = payloadName;
    payload->type = success;
    HirMatchArm continuing;
    continuing.location = location;
    continuing.pattern = NativeCasePattern(operandType, NativeSuccessTag, success, std::move(payload), location);
    continuing.body = TransferredBinding(payloadName, success, location);
    lowered->arms.push_back(std::move(continuing));

    // The failure arm owns the error as the mapper's binder; the mapper runs once and its value leaves as the outer
    // failure through an ordinary return, which destroys the binder unless the mapper moved it.
    HirMatchArm failing;
    failing.location = location;
    PushScope();
    HirPatternPtr binder;
    if (expression.binding == "_") {
        auto ignored = std::make_unique<HirWildcardPattern>();
        ignored->location = expression.bindingLocation;
        binder = std::move(ignored);
    }
    else {
        auto bound = std::make_unique<HirBindingPattern>();
        bound->location = expression.bindingLocation;
        bound->name = expression.binding;
        bound->type = error;
        HirSymbol symbol;
        symbol.kind = HirSymbol::Kind::Var;
        symbol.name = expression.binding;
        symbol.type = error;
        symbol.bindingId = RegisterCleanupBinding(symbol.name, symbol.type, expression.bindingLocation);
        bound->bindingId = symbol.bindingId;
        Define(std::move(symbol));
        binder = std::move(bound);
    }
    failing.pattern = NativeCasePattern(operandType, NativeFailureTag, error, std::move(binder), location);
    if (dynamic_cast<const DivergeExpr *>(expression.mapper.get())) {
        failing.body = LowerExpr(*expression.mapper);
    }
    else {
        const TypeRef channel = returnType.IsFallible() ? returnType.FallibleError() : error;
        auto exit = std::make_unique<HirBlockExpr>();
        exit->location = location;
        exit->type = success;
        exit->block.location = location;
        exit->block.stmts.push_back(LowerFunctionReturn(
            MakeNativeCase(returnType, NativeFailureTag, LowerExprAs(*expression.mapper, channel), location),
            location));
        failing.body = std::move(exit);
    }
    failing.cleanups = CurrentScopeCleanups();
    PopScope();
    lowered->arms.push_back(std::move(failing));
    return lowered;
}

HirExprPtr AstToHirContext::LowerCatch(const CatchExpr &expression) {
    const SourceLocation location = expression.location;
    auto lowered = std::make_unique<HirMatchExpr>();
    lowered->location = location;
    lowered->type = ResolvedExpressionType(expression);
    lowered->subject = LowerMatchSubject(*expression.subject);
    const TypeRef subjectType = lowered->subject->type;
    const TypeRef &success = subjectType.FallibleSuccess();
    const TypeRef &error = subjectType.FallibleError();

    // The success passes through untouched, moved out of the consumed subject.
    const std::string payloadName = std::format("$catch.value.{}", propagationOrdinal++);
    auto payload = std::make_unique<HirBindingPattern>();
    payload->location = location;
    payload->name = payloadName;
    payload->type = success;
    HirMatchArm passThrough;
    passThrough.location = location;
    passThrough.pattern = NativeCasePattern(subjectType, NativeSuccessTag, success, std::move(payload), location);
    passThrough.body = ConvertNative(TransferredBinding(payloadName, success, location), lowered->type, location);
    lowered->arms.push_back(std::move(passThrough));

    // Each recovery arm matches inside the failure channel; its guard still applies to the whole arm.
    const bool armsOwnPayload = lowered->subject->consumption.has_value();
    for (const auto &arm : expression.arms) {
        HirMatchArm recovery;
        recovery.location = arm.location;
        PushScope();
        const bool savedOwnership = patternBindingsOwnPayload;
        patternBindingsOwnPayload = armsOwnPayload;
        const auto *guarded = dynamic_cast<const GuardedPattern *>(arm.pattern.get());
        const Pattern &errorPattern = guarded && guarded->inner ? *guarded->inner : *arm.pattern;
        HirPatternPtr inner = LowerPattern(errorPattern, error);
        HirPatternPtr failure = NativeCasePattern(subjectType, NativeFailureTag, error, std::move(inner), location);
        if (guarded) {
            auto withGuard = std::make_unique<HirGuardedPattern>();
            withGuard->location = guarded->location;
            withGuard->inner = std::move(failure);
            withGuard->guard = LowerExpr(*guarded->guard);
            failure = std::move(withGuard);
        }
        recovery.pattern = std::move(failure);
        patternBindingsOwnPayload = savedOwnership;
        const bool leaves =
            dynamic_cast<const DivergeExpr *>(arm.body.get()) || dynamic_cast<const BlockExpr *>(arm.body.get());
        recovery.body = leaves ? LowerExpr(*arm.body) : LowerExprAs(*arm.body, lowered->type);
        recovery.cleanups = CurrentScopeCleanups();
        PopScope();
        lowered->arms.push_back(std::move(recovery));
    }
    return lowered;
}

HirExprPtr AstToHirContext::LowerNativeOptionalTry(const TryExpr &expression, const TypeRef &operandType) {
    const SourceLocation location = expression.location;
    const TypeRef payload = operandType.inner.front();
    const std::string payloadName = std::format("$try.value.{}", propagationOrdinal++);
    auto bound = std::make_unique<HirBindingPattern>();
    bound->location = location;
    bound->name = payloadName;
    bound->type = payload;
    HirMatchArm present;
    present.location = location;
    present.pattern = NativeCasePattern(operandType, NativePresentTag, payload, std::move(bound), location);
    present.body = TransferredBinding(payloadName, payload, location);

    // Absence leaves as the enclosing optional's absence, or as successful absence, by an ordinary return.
    auto exit = std::make_unique<HirBlockExpr>();
    exit->location = location;
    exit->type = payload;
    exit->block.location = location;
    exit->block.stmts.push_back(LowerFunctionReturn(LowerNoneAs(currentReturnType, location), location));
    HirMatchArm absent;
    absent.location = location;
    absent.pattern = NativeCasePattern(operandType, NativeAbsentTag, std::nullopt, nullptr, location);
    absent.body = std::move(exit);

    auto lowered = std::make_unique<HirMatchExpr>();
    lowered->location = location;
    lowered->type = payload;
    lowered->subject = LowerExpr(*expression.operand);
    lowered->arms.push_back(std::move(present));
    lowered->arms.push_back(std::move(absent));
    return lowered;
}

HirExprPtr AstToHirContext::LowerNativeCoalesce(const BinaryExpr &expression) {
    const SourceLocation location = expression.location;
    const TypeRef operandType = ResolvedExpressionType(*expression.left);
    const TypeRef payload = operandType.inner.front();
    const std::string payloadName = std::format("$coalesce.value.{}", coalescingOrdinal++);
    auto bound = std::make_unique<HirBindingPattern>();
    bound->location = location;
    bound->name = payloadName;
    bound->type = payload;
    HirMatchArm present;
    present.location = location;
    present.pattern = NativeCasePattern(operandType, NativePresentTag, payload, std::move(bound), location);
    present.body = TransferredBinding(payloadName, payload, location);
    HirMatchArm absent;
    absent.location = location;
    absent.pattern = NativeCasePattern(operandType, NativeAbsentTag, std::nullopt, nullptr, location);
    absent.body = dynamic_cast<const DivergeExpr *>(expression.right.get()) ? LowerExpr(*expression.right)
                                                                            : LowerExprAs(*expression.right, payload);

    auto lowered = std::make_unique<HirMatchExpr>();
    lowered->location = location;
    lowered->type = payload;
    lowered->subject = LowerExpr(*expression.left);
    lowered->arms.push_back(std::move(present));
    lowered->arms.push_back(std::move(absent));
    return lowered;
}

HirFunc AstToHirContext::FallibleEntryWrapper(const HirFunc &body) {
    const SourceLocation location = body.location;
    const TypeRef outcome = body.returnType;
    const TypeRef status = TypeRef::MakeInt();
    const auto literal = [&](const std::string &value) {
        auto result = std::make_unique<HirLiteralExpr>();
        result->location = location;
        result->type = status;
        result->value = value;
        return result;
    };

    auto call = std::make_unique<HirCallExpr>();
    call->location = location;
    call->type = outcome;
    auto callee = std::make_unique<HirVarExpr>();
    callee->location = location;
    callee->name = std::string(kFallibleMainBody);
    std::vector<TypeRef> parameterTypes;
    for (const HirParam &parameter : body.params) {
        parameterTypes.push_back(parameter.type);
        auto argument = std::make_unique<HirVarExpr>();
        argument->location = location;
        argument->name = parameter.name;
        argument->type = parameter.type;
        call->args.push_back(std::move(argument));
    }
    callee->type = TypeRef::MakeFunc(std::move(parameterTypes), outcome);
    call->callee = std::move(callee);

    const TypeRef &success = outcome.FallibleSuccess();
    HirMatchArm succeeded;
    succeeded.location = location;
    if (success.IsInteger()) {
        auto value = std::make_unique<HirBindingPattern>();
        value->location = location;
        value->name = "$main.status";
        value->type = success;
        succeeded.pattern = NativeCasePattern(outcome, NativeSuccessTag, success, std::move(value), location);
        auto read = std::make_unique<HirVarExpr>();
        read->location = location;
        read->name = "$main.status";
        read->type = success;
        auto cast = std::make_unique<HirCastExpr>();
        cast->location = location;
        cast->type = status;
        cast->targetType = status;
        cast->operand = std::move(read);
        succeeded.body = std::move(cast);
    }
    else {
        auto ignored = std::make_unique<HirWildcardPattern>();
        ignored->location = location;
        succeeded.pattern = NativeCasePattern(outcome, NativeSuccessTag, success, std::move(ignored), location);
        succeeded.body = literal("0");
    }
    HirMatchArm failed;
    failed.location = location;
    auto ignoredError = std::make_unique<HirWildcardPattern>();
    ignoredError->location = location;
    failed.pattern =
        NativeCasePattern(outcome, NativeFailureTag, outcome.FallibleError(), std::move(ignoredError), location);
    failed.body = literal("1");

    auto match = std::make_unique<HirMatchExpr>();
    match->location = location;
    match->type = status;
    // The outcome is a temporary the entry point owns, so each arm destroys what it leaves unbound.
    call->consumption = ValueConsumptionKind::MatchSubject;
    match->subject = std::move(call);
    match->arms.push_back(std::move(succeeded));
    match->arms.push_back(std::move(failed));

    auto returned = std::make_unique<HirReturnStmt>();
    returned->location = location;
    returned->value = std::move(match);

    HirFunc entry;
    entry.name = body.name;
    entry.isPublic = body.isPublic;
    entry.callConv = body.callConv;
    entry.params = body.params;
    for (HirParam &parameter : entry.params) {
        parameter.bindingId = 0;
    }
    entry.returnType = status;
    entry.location = location;
    entry.body = HirBlock{};
    entry.body->location = location;
    entry.body->stmts.push_back(std::move(returned));
    return entry;
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
