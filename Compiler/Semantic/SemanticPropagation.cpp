// Failure propagation: what `expr?` may be written on, what it evaluates to, and what the enclosing function has to
// return for the failure it propagates to be returnable at all.

#include "Semantic/Detail/SemanticAnalyzerContext.h"

#include <format>

namespace Rux::SemanticDetail {
namespace {
/// The variants a propagatable type is recognized by. Neither `Result` nor `Option` is built in: both are ordinary
/// generic enums, so the compiler identifies them by the shape it has to generate an early return for -- a success
/// variant carrying the value and a failure variant carrying whatever the caller receives.
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
} // namespace

std::optional<SemanticAnalyzerContext::PropagationShape>
SemanticAnalyzerContext::PropagationShapeOf(const TypeRef &type) const {
    if (type.kind != TypeRef::Kind::Named) {
        return std::nullopt;
    }
    const std::string baseName = BaseTypeName(type.name);
    const auto declaration = enumDecls.find(baseName);
    if (declaration == enumDecls.end()) {
        return std::nullopt;
    }

    const EnumDecl &enumeration = *declaration->second;
    const bool isResult = FindVariant(enumeration, kResultSuccess) && FindVariant(enumeration, kResultError);
    const bool isOption = FindVariant(enumeration, kOptionSome) && FindVariant(enumeration, kOptionNone);
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
    // The payload and the failure travel as the type arguments of their own variants, so a `Result` with no arguments
    // left to name -- the generic declaration read inside itself -- has neither, and propagation stays unresolved
    // rather than guessing at one.
    if (!arguments.empty()) {
        shape.payload = arguments.front();
    }
    if (shape.kind == PropagationShape::Kind::Result && arguments.size() >= 2) {
        shape.failure = arguments[1];
    }
    return shape;
}

std::string_view SemanticAnalyzerContext::PropagationKindName(const PropagationShape::Kind kind) {
    return kind == PropagationShape::Kind::Result ? "Result" : "Option";
}

std::string_view SemanticAnalyzerContext::PropagationKindPhrase(const PropagationShape::Kind kind) {
    return kind == PropagationShape::Kind::Result ? "a Result" : "an Option";
}

std::optional<TypeRef> SemanticAnalyzerContext::CheckTryExpression(const TryExpr &expression) {
    const TypeRef operandType = CheckExpr(*expression.operand);
    if (operandType.IsUnknown()) {
        return TypeRef::MakeUnknown();
    }

    const auto operand = PropagationShapeOf(operandType);
    if (!operand) {
        EmitError(expression.location,
                  std::format("'{}' cannot be propagated with '?' because it is neither a Result nor an Option",
                              operandType.ToString()),
                  {}, "'?' propagates a 'Result<T, E>' or an 'Option<T>'");
        return TypeRef::MakeUnknown();
    }

    const std::string_view operandKind = PropagationKindPhrase(operand->kind);
    const auto enclosing = PropagationShapeOf(currentReturnType);
    if (!enclosing) {
        const std::string returned = currentReturnType.kind == TypeRef::Kind::Opaque
                                       ? "nothing"
                                       : std::format("'{}'", currentReturnType.ToString());
        EmitError(expression.location,
                  std::format("'?' propagates {}, but the enclosing function returns {}", operandKind, returned), {},
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
    propagation.enumName = operand->declaration->name;
    propagation.successVariant = propagation.isResult ? std::string(kResultSuccess) : std::string(kOptionSome);
    propagation.failureVariant = propagation.isResult ? std::string(kResultError) : std::string(kOptionNone);
    propagation.returnEnumName = enclosing->declaration->name;
    propagation.payloadType = operand->payload;
    propagation.failureType = operand->failure;
    propagation.returnType = currentReturnType;
    propagations.insert_or_assign(&expression, std::move(propagation));
    return operand->payload;
}
} // namespace Rux::SemanticDetail
