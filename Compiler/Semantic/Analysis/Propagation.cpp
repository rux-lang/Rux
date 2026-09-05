// Failure propagation: what `expr?` may be written on, what it evaluates to, and what the enclosing function has to
// return for the failure it propagates to be returnable at all.

#include "Semantic/Analysis/AnalysisContext.h"

#include <format>
#include <unordered_map>

namespace Rux::SemanticDetail {
namespace {
/// The cases a propagatable type is recognized by. Neither `Result` nor `Option` is built in: both are ordinary
/// variants, so the compiler identifies them by the shape it has to generate an early return for -- a success case
/// carrying the value and a failure case carrying whatever the caller receives.
constexpr std::string_view kResultSuccess = "Success";
constexpr std::string_view kResultError = "Error";
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

enum class ProtocolKind {
    Result,
    Option
};

[[nodiscard]] std::optional<ProtocolKind> ProtocolKindOf(const EnumDecl &declaration) {
    if (FindVariant(declaration, kResultSuccess) || FindVariant(declaration, kResultError)) {
        return ProtocolKind::Result;
    }
    if (FindVariant(declaration, kOptionSome) || FindVariant(declaration, kOptionNone)) {
        return ProtocolKind::Option;
    }
    return std::nullopt;
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
    const EnumDecl::Variant *success = FindVariant(enumeration, kResultSuccess);
    const EnumDecl::Variant *error = FindVariant(enumeration, kResultError);
    const EnumDecl::Variant *some = FindVariant(enumeration, kOptionSome);
    const EnumDecl::Variant *none = FindVariant(enumeration, kOptionNone);
    const bool isResult = success && error && IsSinglePayloadCase(*success) && IsSinglePayloadCase(*error);
    const bool isOption = some && none && IsSinglePayloadCase(*some) && IsUnitCase(*none);
    if (!isResult && !isOption) {
        return std::nullopt;
    }

    const std::vector<TypeRef> arguments = ParseTypeArgsFromTypeName(type.name);
    if (arguments.size() != enumeration.typeParams.size()) {
        return std::nullopt;
    }

    PropagationShape shape;
    shape.declaration = &enumeration;
    shape.kind = isResult ? PropagationShape::Kind::Result : PropagationShape::Kind::Option;

    // A generic declaration carries the payload and the failure as its type arguments; one written for a single type
    // carries them as the fields of the variants themselves. Reading the fields covers both, because a generic
    // variant's field is the parameter the argument substitutes.
    std::unordered_map<std::string, TypeRef> substitutions;
    for (std::size_t index = 0; index < arguments.size() && index < enumeration.typeParams.size(); ++index) {
        substitutions.emplace(enumeration.typeParams[index].name, arguments[index]);
    }
    const auto payloadOf = [&](const std::string_view variantName) -> std::optional<TypeRef> {
        const EnumDecl::Variant *variant = FindVariant(enumeration, variantName);
        if (!variant || variant->fields.empty()) {
            return std::nullopt;
        }
        return ResolveTypeWithSubstitution(*variant->fields.front(), substitutions);
    };
    if (const auto payload = payloadOf(shape.kind == PropagationShape::Kind::Result ? kResultSuccess : kOptionSome)) {
        shape.payload = *payload;
    }
    if (shape.kind == PropagationShape::Kind::Result) {
        shape.failure = payloadOf(kResultError);
    }
    return shape;
}

std::optional<std::string>
AnalysisContext::PropagationShapeIssue(const TypeRef &type,
                                       const std::optional<PropagationShape::Kind> expectedKind) const {
    if (type.kind != TypeRef::Kind::Named) {
        return std::nullopt;
    }
    const EnumDecl *declaration = EnumNamed(BaseTypeName(type.name));
    if (!declaration) {
        return std::nullopt;
    }
    const auto kind = ProtocolKindOf(*declaration);
    if (!kind) {
        return std::nullopt;
    }
    const PropagationShape::Kind shapeKind =
        *kind == ProtocolKind::Result ? PropagationShape::Kind::Result : PropagationShape::Kind::Option;
    if (expectedKind && *expectedKind != shapeKind) {
        return std::nullopt;
    }
    const std::string_view protocol = shapeKind == PropagationShape::Kind::Result ? "Result" : "Option";
    if (!declaration->IsVariant()) {
        return std::format("type '{}' uses a scalar enum for the {} protocol; declare it with 'variant'",
                           type.ToString(), protocol);
    }
    const std::string_view cases = shapeKind == PropagationShape::Kind::Result
                                     ? "exactly 'Success(T)' and 'Error(E)'"
                                     : "exactly 'Some(T)' and payload-less 'None'";
    return std::format("type '{}' is not a valid {} variant; expected {} cases", type.ToString(), protocol, cases);
}

std::string_view AnalysisContext::PropagationKindName(const PropagationShape::Kind kind) {
    return kind == PropagationShape::Kind::Result ? "Result" : "Option";
}

std::string_view AnalysisContext::PropagationKindPhrase(const PropagationShape::Kind kind) {
    return kind == PropagationShape::Kind::Result ? "a Result" : "an Option";
}

std::optional<TypeRef> AnalysisContext::CheckTryExpression(const TryExpr &expression) {
    const TypeRef operandType = CheckExpr(*expression.operand);
    if (operandType.IsUnknown()) {
        return TypeRef::MakeUnknown();
    }

    const auto operand = PropagationShapeOf(operandType);
    if (!operand) {
        std::vector<std::string> notes;
        if (auto issue = PropagationShapeIssue(operandType)) {
            notes.push_back(std::move(*issue));
        }
        EmitError(expression.location,
                  std::format("'{}' cannot be propagated with '?' because it is neither a Result nor an Option",
                              operandType.ToString()),
                  std::move(notes), "'?' propagates a variant shaped as 'Result<T, E>' or 'Option<T>'");
        return TypeRef::MakeUnknown();
    }

    const std::string_view operandKind = PropagationKindPhrase(operand->kind);
    const auto enclosing = PropagationShapeOf(currentReturnType);
    if (!enclosing) {
        std::vector<std::string> notes;
        if (auto issue = PropagationShapeIssue(currentReturnType)) {
            notes.push_back(std::move(*issue));
        }
        const std::string returned = currentReturnType.kind == TypeRef::Kind::Opaque
                                       ? "nothing"
                                       : std::format("'{}'", currentReturnType.ToString());
        EmitError(expression.location,
                  std::format("'?' propagates {}, but the enclosing function returns {}", operandKind, returned),
                  std::move(notes),
                  std::format("give the function a '{}' return type, or handle the failure with 'match'",
                              PropagationKindName(operand->kind)));
        return operand->payload;
    }

    if (enclosing->kind != operand->kind) {
        EmitError(expression.location,
                  std::format("'?' propagates {}, but the enclosing function returns '{}'", operandKind,
                              currentReturnType.ToString()),
                  {},
                  std::format("convert the {} to a {} before propagating it", PropagationKindName(operand->kind),
                              PropagationKindName(enclosing->kind)));
        return operand->payload;
    }

    // The first release propagates one error type unchanged. Converting between them silently is what makes an error
    // path hard to follow, and every conversion a caller does want is a named call it can write itself.
    if (operand->kind == PropagationShape::Kind::Result && operand->failure && enclosing->failure &&
        !operand->failure->IsUnknown() && !enclosing->failure->IsUnknown() &&
        *operand->failure != *enclosing->failure) {
        EmitError(expression.location,
                  std::format("'?' propagates error type '{}', but the enclosing function returns error type '{}'",
                              operand->failure->ToString(), enclosing->failure->ToString()),
                  {"'?' does not convert between error types"},
                  std::format("map the error to '{}' before propagating it", enclosing->failure->ToString()));
        return operand->payload;
    }

    ResolvedPropagation propagation;
    propagation.isResult = operand->kind == PropagationShape::Kind::Result;
    propagation.variantName = operand->declaration->name;
    propagation.successVariant = propagation.isResult ? std::string(kResultSuccess) : std::string(kOptionSome);
    propagation.failureVariant = propagation.isResult ? std::string(kResultError) : std::string(kOptionNone);
    propagation.returnVariantName = enclosing->declaration->name;
    propagation.payloadType = operand->payload;
    propagation.failureType = operand->failure;
    propagation.returnType = currentReturnType;
    propagations.insert_or_assign(&expression, std::move(propagation));
    return operand->payload;
}

bool AnalysisContext::ValidateCoalescingPayload(const TypeRef &payload, const SourceLocation location) {
    if (payload.IsUnknown() || (!ClassifyTypeProperties(payload).IsResolved() && MentionsTypeParameter(payload))) {
        return true;
    }
    if (payload.kind == TypeRef::Kind::Reference) {
        EmitError(location,
                  std::format("'{}' cannot extract reference payload type '{}' from an Option",
                              "?"
                              "?",
                              payload.ToString()),
                  {"the hidden Some payload has no source place whose borrow provenance can be preserved"},
                  "handle the Option with an explicit match, or store a raw pointer when an address must escape");
        return false;
    }
    const TypeProperties properties = ClassifyTypeProperties(payload);
    if (properties.IsResolved() && !properties.IsMovable()) {
        EmitError(location,
                  std::format("'{}' cannot extract payload type '{}' because moving it is prohibited",
                              "?"
                              "?",
                              payload.ToString()),
                  {"coalescing transfers the Some payload into the result"},
                  "use an explicit match and copy the payload, or permit the canonical move operation");
        return false;
    }
    return true;
}

TypeRef AnalysisContext::CheckCoalesceExpression(const BinaryExpr &expression) {
    const TypeRef leftType = CheckExpr(*expression.left);
    if (leftType.IsUnknown()) {
        static_cast<void>(CheckExpr(*expression.right));
        return TypeRef::MakeUnknown();
    }

    const auto shape = PropagationShapeOf(leftType);
    if (!shape) {
        static_cast<void>(CheckExpr(*expression.right));
        std::vector<std::string> notes;
        if (auto issue = PropagationShapeIssue(leftType, PropagationShape::Kind::Option)) {
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
    if (shape->kind != PropagationShape::Kind::Option) {
        static_cast<void>(CheckExpr(*expression.right));
        EmitError(expression.location,
                  std::format("Result value '{}' cannot be coalesced with '{}'", leftType.ToString(),
                              "?"
                              "?"),
                  {"a Result carries an error that coalescing would silently discard"},
                  "handle Error explicitly with 'match', or convert the Result to an Option first");
        return TypeRef::MakeUnknown();
    }

    bool payloadValid = true;
    if (MentionsTypeParameter(shape->payload) && currentFunctionDecl) {
        deferredCoalescingChecks[currentFunctionDecl].push_back({shape->payload, expression.location});
    }
    else {
        payloadValid = ValidateCoalescingPayload(shape->payload, expression.location);
    }

    ConsumeValue(*expression.left, leftType, ValueConsumptionKind::CoalescingOperand, expression.left->location);
    const TrackedFlow someExit = SaveTrackedFlow();

    const TypeRef rightType = CheckExpr(*expression.right);
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
        coalescings.insert_or_assign(&expression, ResolvedCoalescing{shape->declaration->name, std::string(kOptionSome),
                                                                     std::string(kOptionNone), shape->payload});
    }
    return shape->payload;
}
} // namespace Rux::SemanticDetail
