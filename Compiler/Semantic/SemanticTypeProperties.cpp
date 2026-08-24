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
TypeProperties SemanticAnalyzerContext::ClassifyTypeProperties(const TypeRef &type) {
    const std::string key = type.ToString();
    if (const auto known = typeProperties.find(key); known != typeProperties.end() && known->second.IsResolved()) {
        return known->second;
    }

    TypePropertyClassifier classifier(
        structDecls, enumDecls, unionDecls, interfaceDecls, typeImplementsInterfaces,
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
