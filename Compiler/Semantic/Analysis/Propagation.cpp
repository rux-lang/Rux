// Failure propagation: what `expr?` may be written on, what it evaluates to, and what the enclosing function has to
// return for the failure it propagates to be returnable at all.

#include "Semantic/Analysis/AnalysisContext.h"

#include <algorithm>
#include <format>

namespace Rux::SemanticDetail {
namespace {
[[nodiscard]] bool HasCase(const EnumDecl &declaration, const std::string_view name) {
    return std::ranges::any_of(declaration.variants,
                               [&](const EnumDecl::Variant &variant) { return variant.name == name; });
}
} // namespace

AnalysisContext::LegacyOutcomeShape AnalysisContext::LegacyOutcomeShapeOf(const TypeRef &type) const {
    if (type.kind != TypeRef::Kind::Named) {
        return LegacyOutcomeShape::None;
    }
    const EnumDecl *declaration = EnumNamed(BaseTypeName(type.name));
    if (!declaration || !declaration->IsVariant()) {
        return LegacyOutcomeShape::None;
    }
    if (HasCase(*declaration, "Success") && HasCase(*declaration, "Error")) {
        return LegacyOutcomeShape::Result;
    }
    if (HasCase(*declaration, "Some") && HasCase(*declaration, "None")) {
        return LegacyOutcomeShape::Option;
    }
    return LegacyOutcomeShape::None;
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
        const std::string returned =
            currentReturnType.IsOpaque() ? "nothing" : std::format("'{}'", currentReturnType.ToString());
        EmitError(expression.location,
                  std::format("'?' propagates the absence of '{}', but the enclosing function returns {}",
                              operandType.ToString(), returned),
                  {"absence leaves as an optional's 'none', or as the successful 'none' of 'U? ! F'; '?' never "
                   "invents an error for it"},
                  "declare an optional result, as in '-> T?', or supply a fallback with '?"
                  "?'");
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

    std::vector<std::string> notes;
    std::string help = "'?' propagates a native fallible 'T ! E' or an optional 'T?'";
    switch (LegacyOutcomeShapeOf(operandType)) {
    case LegacyOutcomeShape::Result:
        notes.push_back("a variant with 'Success' and 'Error' cases is an ordinary variant; only a native fallible "
                        "propagates a failure");
        help = "return 'T ! E' and propagate a native fallible, or match the variant";
        break;
    case LegacyOutcomeShape::Option:
        notes.push_back("a variant with 'Some' and 'None' cases is an ordinary variant; only a native optional "
                        "propagates absence");
        help = "return 'T?' and propagate a native optional, or match the variant";
        break;
    case LegacyOutcomeShape::None:
        break;
    }
    EmitError(expression.location,
              std::format("'{}' cannot be propagated with '?' because it is neither a native fallible nor an optional",
                          operandType.ToString()),
              std::move(notes), std::move(help));
    return TypeRef::MakeUnknown();
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
                              propagation ? "an outcome" : "an optional"),
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

    static_cast<void>(CheckExpr(*expression.right));
    std::vector<std::string> notes;
    if (LegacyOutcomeShapeOf(leftType) == LegacyOutcomeShape::Option) {
        notes.push_back("a variant with 'Some' and 'None' cases is an ordinary variant; only a native optional is "
                        "coalesced");
    }
    EmitError(expression.location,
              std::format("operator '{}' requires an optional left operand, but found '{}'",
                          "?"
                          "?",
                          leftType.ToString()),
              std::move(notes), "coalesce a native optional 'T?'");
    return TypeRef::MakeUnknown();
}
} // namespace Rux::SemanticDetail
