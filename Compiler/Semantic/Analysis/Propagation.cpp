// Failure propagation: what `expr?` may be written on, what it evaluates to, and what the enclosing function has to
// return for the failure it propagates to be returnable at all.

#include "Semantic/Analysis/AnalysisContext.h"

#include <format>
#include <unordered_map>

namespace Rux::SemanticDetail {
namespace {
/// The cases the legacy Option protocol is recognized by. `Option` is not built in: it is an ordinary variant, so the
/// compiler identifies it by the shape it has to generate an early return for — a case carrying the value and a
/// payload-less case for absence. Failure has no legacy shape: only a native fallible `T ! E` propagates an error.
constexpr std::string_view kOptionSome = "Some";
constexpr std::string_view kOptionNone = "None";

[[nodiscard]] const EnumDecl::Variant *FindVariant(const EnumDecl &declaration, const std::string_view name) {
    for (const EnumDecl::Variant &variant : declaration.variants) {
        if (variant.name == name) {
            return &variant;
        }
    }
    return nullptr;
}

[[nodiscard]] bool IsUnitCase(const EnumDecl::Variant &variant) {
    return variant.fields.empty() && variant.namedFields.empty();
}

[[nodiscard]] bool IsSinglePayloadCase(const EnumDecl::Variant &variant) {
    return variant.fields.size() == 1 && variant.namedFields.empty();
}

/// Whether a declaration names a case of the legacy Option protocol, and so meant to follow it.
[[nodiscard]] bool ClaimsOptionProtocol(const EnumDecl &declaration) {
    return FindVariant(declaration, kOptionSome) || FindVariant(declaration, kOptionNone);
}

/// Whether a variant is shaped like the retired Result protocol, which earns it a migration note.
[[nodiscard]] bool LooksLikeLegacyResult(const EnumDecl &declaration) {
    return FindVariant(declaration, "Success") && FindVariant(declaration, "Error");
}
} // namespace

std::optional<AnalysisContext::PropagationShape> AnalysisContext::PropagationShapeOf(const TypeRef &type) {
    if (type.kind != TypeRef::Kind::Named) {
        return std::nullopt;
    }
    const std::string baseName = BaseTypeName(type.name);
    const EnumDecl *declaration = EnumNamed(baseName);
    if (!declaration) {
        return std::nullopt;
    }

    const EnumDecl &enumeration = *declaration;
    if (!enumeration.IsVariant() || enumeration.variants.size() != 2) {
        return std::nullopt;
    }
    const EnumDecl::Variant *some = FindVariant(enumeration, kOptionSome);
    const EnumDecl::Variant *none = FindVariant(enumeration, kOptionNone);
    if (!some || !none || !IsSinglePayloadCase(*some) || !IsUnitCase(*none)) {
        return std::nullopt;
    }

    const std::vector<TypeRef> arguments = ParseTypeArgsFromTypeName(type.name);
    if (arguments.size() != enumeration.typeParams.size()) {
        return std::nullopt;
    }

    PropagationShape shape;
    shape.declaration = &enumeration;

    // A generic declaration carries the payload as its type argument; one written for a single type carries it as
    // the field of the case itself. Reading the field covers both, because a generic variant's field is the parameter
    // the argument substitutes.
    std::unordered_map<std::string, TypeRef> substitutions;
    for (std::size_t index = 0; index < arguments.size() && index < enumeration.typeParams.size(); ++index) {
        substitutions.emplace(enumeration.typeParams[index].name, arguments[index]);
    }
    shape.payload = ResolveTypeWithSubstitution(*some->fields.front(), substitutions);
    return shape;
}

std::optional<std::string> AnalysisContext::PropagationShapeIssue(const TypeRef &type) const {
    if (type.kind != TypeRef::Kind::Named) {
        return std::nullopt;
    }
    const EnumDecl *declaration = EnumNamed(BaseTypeName(type.name));
    if (!declaration || !ClaimsOptionProtocol(*declaration)) {
        return std::nullopt;
    }
    if (!declaration->IsVariant()) {
        return std::format("type '{}' uses a scalar enum for the Option protocol; declare it with 'variant'",
                           type.ToString());
    }
    return std::format("type '{}' is not a valid Option variant; expected exactly 'Some(T)' and payload-less 'None' "
                       "cases",
                       type.ToString());
}

bool AnalysisContext::IsLegacyResultShape(const TypeRef &type) const {
    if (type.kind != TypeRef::Kind::Named) {
        return false;
    }
    const EnumDecl *declaration = EnumNamed(BaseTypeName(type.name));
    return declaration && declaration->IsVariant() && LooksLikeLegacyResult(*declaration);
}

bool AnalysisContext::ErrorFitsChannel(const TypeRef &error, const TypeRef &channel) {
    if (error == channel) {
        return true;
    }
    if (!channel.IsSum()) {
        return false;
    }
    const auto member = [&](const TypeRef &candidate) { return std::ranges::contains(channel.inner, candidate); };
    return member(error) || (error.IsSum() && std::ranges::all_of(error.inner, member));
}

TypeRef AnalysisContext::CheckNativeTry(const TryExpr &expression, const TypeRef &operandType) {
    const TypeRef &success = operandType.FallibleSuccess();
    const TypeRef &error = operandType.FallibleError();
    if (!currentReturnType.IsFallible()) {
        const std::string returned =
            currentReturnType.IsOpaque() ? "nothing" : std::format("'{}'", currentReturnType.ToString());
        EmitError(expression.location,
                  std::format("'?' propagates native fallible '{}', but the enclosing function returns {}",
                              operandType.ToString(), returned),
                  {"'?' leaves through the outer failure channel of a fallible function"},
                  "declare the function's error channel, as in '-> T ! E', or handle the failure with 'match'");
        return success;
    }
    const TypeRef &channel = currentReturnType.FallibleError();
    const bool dependent = MentionsTypeParameter(error) || MentionsTypeParameter(channel);
    if (!dependent && !error.IsUnknown() && !channel.IsUnknown() && !ErrorFitsChannel(error, channel)) {
        EmitError(
            expression.location,
            std::format("'?' propagates error type '{}', but the enclosing function fails with '{}'", error.ToString(),
                        channel.ToString()),
            {"'?' moves an error into the outer failure only by identity, sum member injection, or subset "
             "widening; it never converts an error"},
            std::format("map the error to '{}' with '? else (e => ...)', or match the value", channel.ToString()));
        return success;
    }

    // The continuing success and the outgoing error are both moved out of the evaluated operand.
    const auto validatePayload = [&](const TypeRef &payload) {
        if (MentionsTypeParameter(payload) && currentFunctionDecl) {
            deferredOutcomeChecks[currentFunctionDecl].push_back({payload, expression.location, true});
        }
        else {
            static_cast<void>(ValidateOutcomePayload(payload, expression.location, true));
        }
    };
    validatePayload(success);
    validatePayload(error);
    ConsumeValue(*expression.operand, operandType, ValueConsumptionKind::PropagationOperand,
                 expression.operand->location);

    ResolvedPropagation propagation;
    propagation.native = true;
    propagation.payloadType = success;
    propagation.failureType = error;
    propagation.returnType = currentReturnType;
    propagations.insert_or_assign(&expression, std::move(propagation));
    return success;
}

TypeRef AnalysisContext::CheckNativeOptionalTry(const TryExpr &expression, const TypeRef &operandType) {
    const TypeRef &payload = operandType.inner.front();
    // Absence leaves as the enclosing optional's absence, or as the successful absence of `U? ! F`; nothing deeper is
    // a target, and no error is ever invented for it.
    const bool optionalTarget = currentReturnType.IsOptional();
    const bool successfulAbsence = currentReturnType.IsFallible() && currentReturnType.FallibleSuccess().IsOptional();
    if (!optionalTarget && !successfulAbsence) {
        const bool legacy = PropagationShapeOf(currentReturnType).has_value();
        const std::string returned =
            currentReturnType.IsOpaque() ? "nothing" : std::format("'{}'", currentReturnType.ToString());
        EmitError(expression.location,
                  std::format("'?' propagates the absence of '{}', but the enclosing function returns {}",
                              operandType.ToString(), returned),
                  {"absence leaves as an optional's 'none', or as the successful 'none' of 'U? ! F'; '?' never "
                   "invents an error for it"},
                  legacy ? std::optional<std::string>("native and legacy outcomes do not convert; match the value and "
                                                      "return 'Option::Some(...)' or 'Option::None' explicitly")
                         : std::optional<std::string>("declare an optional result, as in '-> T?', or supply a "
                                                      "fallback with '?"
                                                      "?'"));
        return payload;
    }
    if (MentionsTypeParameter(payload) && currentFunctionDecl) {
        deferredOutcomeChecks[currentFunctionDecl].push_back({payload, expression.location, true});
    }
    else {
        static_cast<void>(ValidateOutcomePayload(payload, expression.location, true));
    }
    ConsumeValue(*expression.operand, operandType, ValueConsumptionKind::PropagationOperand,
                 expression.operand->location);

    ResolvedPropagation propagation;
    propagation.native = true;
    propagation.payloadType = payload;
    propagation.returnType = currentReturnType;
    propagations.insert_or_assign(&expression, std::move(propagation));
    return payload;
}

std::optional<TypeRef> AnalysisContext::CheckTryExpression(const TryExpr &expression) {
    const TypeRef operandType = CheckExpr(*expression.operand);
    if (operandType.IsUnknown()) {
        return TypeRef::MakeUnknown();
    }
    // A borrowed outcome cannot hand its payload to the continuing expression or its error to the caller.
    if (operandType.kind == TypeRef::Kind::Reference && !operandType.inner.empty() &&
        (operandType.inner.front().IsFallible() || operandType.inner.front().IsOptional())) {
        EmitError(expression.location,
                  std::format("'?' cannot consume the borrowed value '{}'", operandType.ToString()),
                  {"'?' moves the success onward or the error out of the function, and a reference owns neither"},
                  "match the borrowed value to inspect it, or propagate an owned value");
        return TypeRef::MakeUnknown();
    }
    if (operandType.IsFallible()) {
        return CheckNativeTry(expression, operandType);
    }
    if (operandType.IsOptional()) {
        return CheckNativeOptionalTry(expression, operandType);
    }

    const auto operand = PropagationShapeOf(operandType);
    if (!operand) {
        std::vector<std::string> notes;
        if (auto issue = PropagationShapeIssue(operandType)) {
            notes.push_back(std::move(*issue));
        }
        const bool resultShaped = IsLegacyResultShape(operandType);
        if (resultShaped) {
            notes.push_back("a variant with 'Success' and 'Error' cases is an ordinary variant; only a native "
                            "fallible propagates a failure");
        }
        EmitError(expression.location,
                  std::format("'{}' cannot be propagated with '?' because it is neither a native fallible nor an "
                              "optional",
                              operandType.ToString()),
                  std::move(notes),
                  resultShaped ? "return 'T ! E' and propagate a native fallible, or match the variant"
                               : "'?' propagates a native fallible 'T ! E' or an optional 'T?'");
        return TypeRef::MakeUnknown();
    }

    ReportLegacyProtocol(expression.location,
                         std::format("'?' uses the legacy Option protocol on '{}'; migrate to a native optional",
                                     operandType.ToString()),
                         "return 'T?' and propagate a native optional");

    const auto enclosing = PropagationShapeOf(currentReturnType);
    if (!enclosing) {
        std::vector<std::string> notes;
        if (auto issue = PropagationShapeIssue(currentReturnType)) {
            notes.push_back(std::move(*issue));
        }
        const std::string returned = currentReturnType.kind == TypeRef::Kind::Opaque
                                       ? "nothing"
                                       : std::format("'{}'", currentReturnType.ToString());
        const bool native = currentReturnType.IsFallible() || currentReturnType.IsOptional();
        EmitError(expression.location,
                  std::format("'?' propagates an Option, but the enclosing function returns {}", returned),
                  std::move(notes),
                  native ? std::string("native and legacy outcomes do not convert; match the Option and return "
                                       "'.Some(...)' or 'none' explicitly")
                         : std::string("give the function an 'Option' return type, or handle the absence with "
                                       "'match'"));
        return operand->payload;
    }

    if (MentionsTypeParameter(operand->payload) && currentFunctionDecl) {
        deferredOutcomeChecks[currentFunctionDecl].push_back({operand->payload, expression.location, true});
    }
    else {
        static_cast<void>(ValidateOutcomePayload(operand->payload, expression.location, true));
    }
    ConsumeValue(*expression.operand, operandType, ValueConsumptionKind::PropagationOperand,
                 expression.operand->location);

    ResolvedPropagation propagation;
    propagation.variantName = programIndex.NominalName(*operand->declaration);
    propagation.successVariant = std::string(kOptionSome);
    propagation.failureVariant = std::string(kOptionNone);
    propagation.returnVariantName = programIndex.NominalName(*enclosing->declaration);
    propagation.payloadType = operand->payload;
    propagation.returnType = currentReturnType;
    propagations.insert_or_assign(&expression, std::move(propagation));
    return operand->payload;
}

bool AnalysisContext::ValidateOutcomePayload(const TypeRef &payload, const SourceLocation location,
                                             const bool propagation) {
    const std::string_view operation = propagation ? "?"
                                                   : "?"
                                                     "?";
    if (payload.IsUnknown() || (!ClassifyTypeProperties(payload).IsResolved() && MentionsTypeParameter(payload))) {
        return true;
    }
    if (payload.kind == TypeRef::Kind::Reference) {
        EmitError(location,
                  std::format("'{}' cannot extract reference payload type '{}' from {}", operation, payload.ToString(),
                              propagation ? "an outcome" : "an Option"),
                  {"the hidden payload has no source place whose borrow provenance can be preserved"},
                  "handle the outcome with an explicit match, or store a raw pointer when an address must escape");
        return false;
    }
    const TypeProperties properties = ClassifyTypeProperties(payload);
    if (properties.IsResolved() && !properties.IsMovable()) {
        EmitError(location,
                  std::format("'{}' cannot extract payload type '{}' because moving it is prohibited", operation,
                              payload.ToString()),
                  {"extracting an outcome transfers its active payload into the result"},
                  "use an explicit match and copy the payload, or permit the canonical move operation");
        return false;
    }
    return true;
}

TypeRef AnalysisContext::CheckNativeCoalesce(const BinaryExpr &expression, const TypeRef &leftType) {
    const TypeRef &payload = leftType.inner.front();
    bool payloadValid = true;
    if (MentionsTypeParameter(payload) && currentFunctionDecl) {
        deferredOutcomeChecks[currentFunctionDecl].push_back({payload, expression.location});
    }
    else {
        payloadValid = ValidateOutcomePayload(payload, expression.location);
    }
    ConsumeValue(*expression.left, leftType, ValueConsumptionKind::CoalescingOperand, expression.left->location);
    const TrackedFlow presentExit = SaveTrackedFlow();

    // The fallback runs only for absence, and either converts to the payload type or leaves.
    const TypeRef checkedFallback = CheckExpr(*expression.right);
    const TypeRef rightType = IsDivergingExpression(*expression.right) ? TypeRef::MakeUnknown() : checkedFallback;
    const bool fallbackValid =
        rightType.IsUnknown() || payload.IsUnknown() || CanAssignExprTo(*expression.right, rightType, payload);
    if (!fallbackValid) {
        EmitError(expression.right->location,
                  AssignmentErrorMessage(*expression.right, payload,
                                         std::format("coalescing fallback has type '{}', but the optional payload is "
                                                     "'{}'",
                                                     rightType.ToString(), payload.ToString())));
    }
    else if (!rightType.IsUnknown()) {
        ConsumeValue(*expression.right, rightType, ValueConsumptionKind::CoalescingFallback,
                     expression.right->location);
    }
    const TrackedFlow absentExit = SaveTrackedFlow();
    MergeTrackedFlows({presentExit, absentExit});
    if (payloadValid && fallbackValid && !payload.IsUnknown()) {
        ResolvedCoalescing coalescing;
        coalescing.native = true;
        coalescing.payloadType = payload;
        coalescings.insert_or_assign(&expression, std::move(coalescing));
    }
    return payload;
}

TypeRef AnalysisContext::CheckCoalesceExpression(const BinaryExpr &expression) {
    const TypeRef leftType = CheckExpr(*expression.left);
    if (leftType.IsUnknown()) {
        static_cast<void>(CheckExpr(*expression.right));
        return TypeRef::MakeUnknown();
    }
    if (leftType.IsOptional() && !leftType.IsIncompleteNative()) {
        return CheckNativeCoalesce(expression, leftType);
    }
    if (leftType.IsFallible() || (leftType.kind == TypeRef::Kind::Reference && !leftType.inner.empty() &&
                                  (leftType.inner.front().IsOptional() || leftType.inner.front().IsFallible()))) {
        static_cast<void>(CheckExpr(*expression.right));
        const bool reference = leftType.kind == TypeRef::Kind::Reference;
        EmitError(expression.location,
                  std::format("operator '{}' cannot take '{}'",
                              "?"
                              "?",
                              leftType.ToString()),
                  {reference ? std::string("coalescing consumes its operand, and a reference owns nothing to consume")
                             : std::string("coalescing tests one optional level and would silently discard an "
                                           "error")},
                  reference ? std::string("match the borrowed value, or coalesce an owned optional")
                            : std::string("recover the error with 'catch', or propagate it with '?'"));
        return TypeRef::MakeUnknown();
    }

    const auto shape = PropagationShapeOf(leftType);
    if (!shape) {
        static_cast<void>(CheckExpr(*expression.right));
        std::vector<std::string> notes;
        if (auto issue = PropagationShapeIssue(leftType)) {
            notes.push_back(std::move(*issue));
        }
        EmitError(expression.location,
                  std::format("operator '{}' requires an Option-shaped left operand, but found '{}'",
                              "?"
                              "?",
                              leftType.ToString()),
                  std::move(notes), "use a variant with exactly 'Some(T)' and payload-less 'None' cases");
        return TypeRef::MakeUnknown();
    }

    ReportLegacyProtocol(expression.location,
                         std::format("'{}' uses the legacy Option protocol on '{}'; migrate to a native optional",
                                     "?"
                                     "?",
                                     leftType.ToString()),
                         "coalesce a native optional 'T?'");

    bool payloadValid = true;
    if (MentionsTypeParameter(shape->payload) && currentFunctionDecl) {
        deferredOutcomeChecks[currentFunctionDecl].push_back({shape->payload, expression.location});
    }
    else {
        payloadValid = ValidateOutcomePayload(shape->payload, expression.location);
    }

    ConsumeValue(*expression.left, leftType, ValueConsumptionKind::CoalescingOperand, expression.left->location);
    const TrackedFlow someExit = SaveTrackedFlow();

    const TypeRef checkedFallback = CheckExpr(*expression.right);
    const TypeRef rightType = IsDivergingExpression(*expression.right) ? TypeRef::MakeUnknown() : checkedFallback;
    bool fallbackValid = rightType.IsUnknown() || shape->payload.IsUnknown() ||
                         CanAssignExprTo(*expression.right, rightType, shape->payload);
    if (!fallbackValid) {
        EmitError(
            expression.right->location,
            AssignmentErrorMessage(*expression.right, shape->payload,
                                   std::format("coalescing fallback has type '{}', but the Option payload is '{}'",
                                               rightType.ToString(), shape->payload.ToString())));
    }
    else if (!rightType.IsUnknown()) {
        ConsumeValue(*expression.right, rightType, ValueConsumptionKind::CoalescingFallback,
                     expression.right->location);
    }
    const TrackedFlow noneExit = SaveTrackedFlow();
    MergeTrackedFlows({someExit, noneExit});

    if (payloadValid && fallbackValid && !shape->payload.IsUnknown()) {
        coalescings.insert_or_assign(&expression, ResolvedCoalescing{programIndex.NominalName(*shape->declaration),
                                                                     std::string(kOptionSome), std::string(kOptionNone),
                                                                     shape->payload});
    }
    return shape->payload;
}
} // namespace Rux::SemanticDetail
