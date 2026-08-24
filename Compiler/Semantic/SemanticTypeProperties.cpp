#include "Semantic/Detail/SemanticAnalyzerContext.h"
#include "Semantic/Detail/TypePropertyClassifier.h"

namespace Rux {
std::string_view ValueConsumptionKindName(const ValueConsumptionKind kind) noexcept {
    switch (kind) {
    case ValueConsumptionKind::Initialization:
        return "initialization";
    case ValueConsumptionKind::Argument:
        return "argument";
    case ValueConsumptionKind::Receiver:
        return "receiver";
    case ValueConsumptionKind::Return:
        return "return";
    case ValueConsumptionKind::Assignment:
        return "assignment";
    case ValueConsumptionKind::Aggregate:
        return "aggregate";
    case ValueConsumptionKind::ConditionalArm:
        return "conditional-arm";
    case ValueConsumptionKind::ExplicitMove:
        return "explicit";
    }
    return "unknown";
}
} // namespace Rux

namespace Rux::SemanticDetail {
bool SemanticAnalyzerContext::IsSpecialOperationName(const std::string_view name) {
    return name == "=" || name == "<-";
}

void SemanticAnalyzerContext::ValidateSpecialOperation(const FuncDecl &method, const TypeRef &extendedType) {
    if (!IsSpecialOperationName(method.name)) {
        return;
    }

    const auto sameValueType = [](TypeRef left, TypeRef right) {
        left.isMut = false;
        right.isMut = false;
        return left == right;
    };
    const bool copy = method.name == "=";
    const auto canonicalSignature = [&] {
        if (!method.typeParams.empty() || method.params.size() != 2 || method.returnType ||
            method.params[0].name != "self" || method.params[1].name != "other" || method.params[0].isMut ||
            method.params[1].isMut || method.params[0].isVariadic || method.params[1].isVariadic ||
            method.params[0].defaultValue || method.params[1].defaultValue) {
            return false;
        }

        const TypeRef receiver = ResolveType(*method.params[0].type);
        const TypeRef other = ResolveType(*method.params[1].type);
        if (receiver.kind != TypeRef::Kind::Reference || receiver.inner.empty() || !receiver.inner.front().isMut ||
            !sameValueType(receiver.inner.front(), extendedType)) {
            return false;
        }
        if (!copy) {
            return other.kind != TypeRef::Kind::Reference && sameValueType(other, extendedType);
        }
        return other.kind == TypeRef::Kind::Reference && !other.inner.empty() && !other.inner.front().isMut &&
               sameValueType(other.inner.front(), extendedType);
    }();
    if (canonicalSignature) {
        return;
    }

    const std::string type = extendedType.ToString();
    const std::string expected = copy ? std::format("func =(self: &var {0}, other: &{0})", type)
                                      : std::format("func <-(self: &var {0}, other: {0})", type);
    EmitError(method.location, std::format("{} special operation for type '{}' must have signature '{}'",
                                           copy ? "copy" : "move", type, expected));
}

TypeProperties SemanticAnalyzerContext::ClassifyTypeProperties(const TypeRef &type) {
    const std::string key = type.ToString();
    if (const auto known = typeProperties.find(key); known != typeProperties.end() && known->second.IsResolved()) {
        return known->second;
    }

    TypePropertyClassifier classifier(
        structDecls, enumDecls, unionDecls, interfaceDecls, typeImplementsInterfaces, methodsByType,
        [this](const TypeExpr &type, const TypePropertyClassifier::Substitutions &substitutions) {
            if (!typeNodeTypes.contains(&type)) {
                return TypeRef::MakeUnknown();
            }
            return ResolveTypeWithSubstitution(type, substitutions);
        },
        [this](const std::string &name) { return ParseTypeArgsFromTypeName(name); });
    const TypeProperties properties = classifier.Classify(type);
    if (properties.IsResolved()) {
        typeProperties.insert_or_assign(key, properties);
    }
    return properties;
}

void SemanticAnalyzerContext::RecordResolvedTypeProperties() {
    const auto record = [&](const TypeRef &type) {
        const TypeProperties properties = ClassifyTypeProperties(type);
        if (!properties.IsResolved()) {
            typeProperties.insert_or_assign(type.ToString(), properties);
        }
    };
    for (const auto &[_, type] : expressionTypes) {
        record(type);
    }
    for (const auto &[_, type] : typeNodeTypes) {
        record(type);
    }
    for (const auto &[_, type] : patternTypes) {
        record(type);
    }
    for (const auto &[name, declaration] : structDecls) {
        if (declaration->typeParams.empty()) {
            record(TypeRef::MakeNamed(name));
        }
    }
    for (const auto &[name, declaration] : enumDecls) {
        if (declaration->typeParams.empty()) {
            record(TypeRef::MakeNamed(name));
        }
    }
    for (const auto &[name, _] : unionDecls) {
        record(TypeRef::MakeNamed(name));
    }
}
} // namespace Rux::SemanticDetail
