#pragma once

#include "Semantic/SemanticModel.h"
#include "Semantic/TypeProperties.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Rux::SemanticDetail {
/// Recursively derives value semantics from declarations after program indexing has completed.
class TypePropertyClassifier {
public:
    using Substitutions = std::unordered_map<std::string, TypeRef>;
    using ResolveType = std::function<TypeRef(const TypeExpr &, const Substitutions &)>;
    using ParseTypeArguments = std::function<std::vector<TypeRef>(const std::string &)>;
    using MethodsByType =
        std::unordered_map<std::string, std::unordered_map<std::string, std::vector<const FuncDecl *>>>;

    TypePropertyClassifier(
        const std::unordered_map<std::string, const StructDecl *> &inputStructs,
        const std::unordered_map<std::string, const EnumDecl *> &inputEnums,
        const std::unordered_map<std::string, const UnionDecl *> &inputUnions,
        const std::unordered_map<std::string, const InterfaceDecl *> &inputInterfaces,
        const std::unordered_map<std::string, std::unordered_set<std::string>> &inputInterfacesByType,
        const MethodsByType &inputMethodsByType, ResolveType inputResolveType,
        ParseTypeArguments inputParseTypeArguments);

    /// Classifies one concrete type, caching recursive aggregate results for subsequent queries.
    [[nodiscard]] TypeProperties Classify(const TypeRef &type);

private:
    const std::unordered_map<std::string, const StructDecl *> &structs;
    const std::unordered_map<std::string, const EnumDecl *> &enums;
    const std::unordered_map<std::string, const UnionDecl *> &unions;
    const std::unordered_map<std::string, const InterfaceDecl *> &interfaces;
    const std::unordered_map<std::string, std::unordered_set<std::string>> &interfacesByType;
    const MethodsByType &methodsByType;
    ResolveType resolveType;
    ParseTypeArguments parseTypeArguments;
    std::unordered_map<std::string, TypeProperties> cache;
    std::unordered_set<std::string> activeTypes;

    [[nodiscard]] TypeProperties ClassifyNamed(const TypeRef &type);
    [[nodiscard]] TypeProperties ClassifyStruct(const StructDecl &declaration, const std::vector<TypeRef> &arguments);
    [[nodiscard]] TypeProperties ClassifyEnum(const EnumDecl &declaration, const std::vector<TypeRef> &arguments);
    [[nodiscard]] TypeProperties ClassifyUnion(const UnionDecl &declaration);
    [[nodiscard]] bool ImplementsDrop(const std::string &baseName) const;
    [[nodiscard]] bool DeclaresDestructor(const TypeRef &type, const std::vector<TypeRef> &arguments) const;
    [[nodiscard]] std::optional<TypeProperties::SpecialOperationState>
    DeclaredSpecialOperation(const TypeRef &type, const std::vector<TypeRef> &arguments, std::string_view name) const;
    [[nodiscard]] bool IsCanonicalSpecialOperation(const FuncDecl &method, const TypeRef &type,
                                                   const Substitutions &substitutions, bool copy) const;
    [[nodiscard]] static std::string BaseTypeName(const std::string &name);
    [[nodiscard]] static bool SameValueType(TypeRef left, TypeRef right);
    [[nodiscard]] static TypeProperties Combine(TypeProperties aggregate, TypeProperties member);
    [[nodiscard]] static Substitutions BindArguments(const std::vector<std::string> &parameters,
                                                     const std::vector<TypeRef> &arguments);
};
} // namespace Rux::SemanticDetail
