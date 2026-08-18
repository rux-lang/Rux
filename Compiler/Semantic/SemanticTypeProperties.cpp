#include "Semantic/Detail/SemanticAnalyzerContext.h"
#include "Semantic/Detail/TypePropertyClassifier.h"

namespace Rux::SemanticDetail {
void SemanticAnalyzerContext::RecordResolvedTypeProperties() {
    TypePropertyClassifier classifier(
        structDecls, enumDecls, unionDecls, interfaceDecls, typeImplementsInterfaces,
        [this](const TypeExpr &type, const TypePropertyClassifier::Substitutions &substitutions) {
            if (!typeNodeTypes.contains(&type)) {
                return TypeRef::MakeUnknown();
            }
            return ResolveTypeWithSubstitution(type, substitutions);
        },
        [this](const std::string &name) { return ParseTypeArgsFromTypeName(name); });

    const auto record = [&](const TypeRef &type) {
        typeProperties.insert_or_assign(type.ToString(), classifier.Classify(type));
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
