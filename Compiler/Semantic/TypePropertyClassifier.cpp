#include "Semantic/Detail/TypePropertyClassifier.h"

#include <algorithm>
#include <utility>

namespace Rux::SemanticDetail {
TypePropertyClassifier::TypePropertyClassifier(
    const std::unordered_map<std::string, const StructDecl *> &inputStructs,
    const std::unordered_map<std::string, const EnumDecl *> &inputEnums,
    const std::unordered_map<std::string, const UnionDecl *> &inputUnions,
    const std::unordered_map<std::string, const InterfaceDecl *> &inputInterfaces,
    const std::unordered_map<std::string, std::unordered_set<std::string>> &inputInterfacesByType,
    ResolveType inputResolveType, ParseTypeArguments inputParseTypeArguments)
    : structs(inputStructs)
    , enums(inputEnums)
    , unions(inputUnions)
    , interfaces(inputInterfaces)
    , interfacesByType(inputInterfacesByType)
    , resolveType(std::move(inputResolveType))
    , parseTypeArguments(std::move(inputParseTypeArguments)) {
}

TypeProperties TypePropertyClassifier::Classify(const TypeRef &type) {
    switch (type.kind) {
    case TypeRef::Kind::Unknown:
    case TypeRef::Kind::TypeParam:
        return TypeProperties::Unresolved();
    case TypeRef::Kind::Opaque:
    case TypeRef::Kind::Bool8:
    case TypeRef::Kind::Bool16:
    case TypeRef::Kind::Bool32:
    case TypeRef::Kind::Char8:
    case TypeRef::Kind::Char16:
    case TypeRef::Kind::Char32:
    case TypeRef::Kind::Int8:
    case TypeRef::Kind::Int16:
    case TypeRef::Kind::Int32:
    case TypeRef::Kind::Int64:
    case TypeRef::Kind::UInt8:
    case TypeRef::Kind::UInt16:
    case TypeRef::Kind::UInt32:
    case TypeRef::Kind::UInt64:
    case TypeRef::Kind::Int:
    case TypeRef::Kind::UInt:
    case TypeRef::Kind::Float32:
    case TypeRef::Kind::Float64:
    case TypeRef::Kind::Str:
    case TypeRef::Kind::Pointer:
    case TypeRef::Kind::RangeFull:
    case TypeRef::Kind::Func:
        return TypeProperties::Copy();
    case TypeRef::Kind::Array:
    case TypeRef::Kind::Range:
    case TypeRef::Kind::RangeInclusive:
    case TypeRef::Kind::RangeFrom:
    case TypeRef::Kind::RangeTo:
    case TypeRef::Kind::RangeToInclusive:
        return type.inner.empty() ? TypeProperties::Unresolved() : Classify(type.inner.front());
    case TypeRef::Kind::Tuple: {
        TypeProperties result = TypeProperties::Copy();
        for (const TypeRef &element : type.inner) {
            result = Combine(result, Classify(element));
        }
        return result;
    }
    case TypeRef::Kind::Named:
        return ClassifyNamed(type);
    }
    return TypeProperties::Unresolved();
}

TypeProperties TypePropertyClassifier::ClassifyNamed(const TypeRef &type) {
    const std::string key = type.ToString();
    if (const auto known = cache.find(key); known != cache.end()) {
        return known->second;
    }

    const std::string baseName = BaseTypeName(type.name);
    if (ImplementsDrop(baseName)) {
        const TypeProperties result = TypeProperties::MoveOnly(true);
        cache.emplace(key, result);
        return result;
    }

    // Interface objects and slices are borrowed, pointer-shaped views. Their pointee may be move-only without making
    // the view itself move-only.
    if (interfaces.contains(baseName) || baseName == "Slice" || baseName == "MutableSlice") {
        const TypeProperties result = TypeProperties::Copy();
        cache.emplace(key, result);
        return result;
    }

    if (!activeTypes.insert(key).second) {
        return TypeProperties::Unresolved();
    }

    const std::vector<TypeRef> arguments = parseTypeArguments(type.name);
    TypeProperties result = TypeProperties::Unresolved();
    if (const auto structure = structs.find(baseName); structure != structs.end()) {
        result = ClassifyStruct(*structure->second, arguments);
    }
    else if (const auto enumeration = enums.find(baseName); enumeration != enums.end()) {
        result = ClassifyEnum(*enumeration->second, arguments);
    }
    else if (const auto unionType = unions.find(baseName); unionType != unions.end()) {
        result = ClassifyUnion(*unionType->second);
    }
    else if (!type.inner.empty()) {
        result = Classify(type.inner.front());
    }

    activeTypes.erase(key);
    cache.emplace(key, result);
    return result;
}

TypeProperties TypePropertyClassifier::ClassifyStruct(const StructDecl &declaration,
                                                      const std::vector<TypeRef> &arguments) {
    if (arguments.size() != declaration.typeParams.size()) {
        return TypeProperties::Unresolved();
    }

    const Substitutions substitutions = BindArguments(declaration.typeParams, arguments);
    TypeProperties result = TypeProperties::Copy();
    for (const StructDecl::Field &field : declaration.fields) {
        result = Combine(result, Classify(resolveType(*field.type, substitutions)));
    }
    return result;
}

TypeProperties TypePropertyClassifier::ClassifyEnum(const EnumDecl &declaration,
                                                    const std::vector<TypeRef> &arguments) {
    if (arguments.size() != declaration.typeParams.size()) {
        return TypeProperties::Unresolved();
    }

    const Substitutions substitutions = BindArguments(declaration.typeParams, arguments);
    TypeProperties result = TypeProperties::Copy();
    for (const EnumDecl::Variant &variant : declaration.variants) {
        for (const TypeExprPtr &field : variant.fields) {
            result = Combine(result, Classify(resolveType(*field, substitutions)));
        }
        for (const EnumDecl::Variant::NamedField &field : variant.namedFields) {
            result = Combine(result, Classify(resolveType(*field.type, substitutions)));
        }
    }
    return result;
}

TypeProperties TypePropertyClassifier::ClassifyUnion(const UnionDecl &declaration) {
    TypeProperties result = TypeProperties::Copy();
    for (const UnionDecl::Field &field : declaration.fields) {
        result = Combine(result, Classify(resolveType(*field.type, {})));
    }
    return result;
}

bool TypePropertyClassifier::ImplementsDrop(const std::string &baseName) const {
    const auto implementations = interfacesByType.find(baseName);
    if (implementations == interfacesByType.end()) {
        return false;
    }
    return std::ranges::any_of(implementations->second,
                               [](const std::string &name) { return name == "Drop" || name.ends_with("::Drop"); });
}

std::string TypePropertyClassifier::BaseTypeName(const std::string &name) {
    const std::size_t arguments = name.find('<');
    return arguments == std::string::npos ? name : name.substr(0, arguments);
}

TypeProperties TypePropertyClassifier::Combine(TypeProperties aggregate, const TypeProperties member) {
    aggregate.droppable = aggregate.droppable || member.droppable;
    if (aggregate.mobility == TypeProperties::Mobility::MoveOnly ||
        member.mobility == TypeProperties::Mobility::MoveOnly) {
        aggregate.mobility = TypeProperties::Mobility::MoveOnly;
    }
    else if (aggregate.mobility == TypeProperties::Mobility::Unresolved ||
             member.mobility == TypeProperties::Mobility::Unresolved) {
        aggregate.mobility = TypeProperties::Mobility::Unresolved;
    }
    else {
        aggregate.mobility = TypeProperties::Mobility::Copy;
    }
    return aggregate;
}

TypePropertyClassifier::Substitutions TypePropertyClassifier::BindArguments(const std::vector<std::string> &parameters,
                                                                            const std::vector<TypeRef> &arguments) {
    Substitutions substitutions;
    const std::size_t count = std::min(parameters.size(), arguments.size());
    for (std::size_t index = 0; index < count; ++index) {
        substitutions.emplace(parameters[index], arguments[index]);
    }
    return substitutions;
}
} // namespace Rux::SemanticDetail
