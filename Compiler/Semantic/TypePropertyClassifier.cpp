#include "Semantic/Detail/TypePropertyClassifier.h"

#include <algorithm>
#include <utility>

namespace Rux::SemanticDetail {
TypePropertyClassifier::TypePropertyClassifier(
    const std::unordered_map<std::string, const StructDecl *> &inputStructs,
    const std::unordered_map<std::string, const EnumDecl *> &inputEnums,
    const std::unordered_map<std::string, const UnionDecl *> &inputUnions,
    const std::unordered_map<std::string, const InterfaceDecl *> &inputInterfaces,
    const MethodsByType &inputMethodsByType, ResolveType inputResolveType, ParseTypeArguments inputParseTypeArguments)
    : structs(inputStructs)
    , enums(inputEnums)
    , unions(inputUnions)
    , interfaces(inputInterfaces)
    , methodsByType(inputMethodsByType)
    , resolveType(std::move(inputResolveType))
    , parseTypeArguments(std::move(inputParseTypeArguments)) {
}

TypeProperties TypePropertyClassifier::Classify(const TypeRef &type) {
    // Every primitive is a Copy type at every width, so the catalog answers for the whole family.
    if (type.IsPrimitive()) {
        return TypeProperties::Copy();
    }
    switch (type.kind) {
    case TypeRef::Kind::Unknown:
    case TypeRef::Kind::TypeParam:
        return TypeProperties::Unresolved();
    case TypeRef::Kind::Opaque:
    case TypeRef::Kind::Pointer:
    case TypeRef::Kind::Reference:
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
    default:
        // Every primitive was already classified above.
        return TypeProperties::Unresolved();
    }
}

TypeProperties TypePropertyClassifier::ClassifyNamed(const TypeRef &type) {
    const std::string key = type.ToString();
    if (const auto known = cache.find(key); known != cache.end()) {
        return known->second;
    }

    const std::string baseName = BaseTypeName(type.name);
    // Interface objects and slices are borrowed, pointer-shaped views. Their pointee may be move-only without making
    // the view itself move-only.
    if (interfaces.contains(baseName) || baseName == "Slice" || baseName == "MutableSlice") {
        const TypeProperties result = TypeProperties::Copy();
        cache.emplace(key, result);
        return result;
    }

    const std::vector<TypeRef> arguments = parseTypeArguments(type.name);
    if (!activeTypes.insert(key).second) {
        // Imported generic owners may expose only their instantiated element type to this package. Following that
        // element can return to the type currently being classified (for example Value -> Vector<Member> -> Value).
        // The recursive edge contributes the operations declared by that type: absent operations are generated,
        // while explicit custom/prohibited declarations and a direct destructor remain authoritative. Invalid
        // infinitely sized value recursion is rejected separately by layout validation.
        const auto copy =
            DeclaredSpecialOperation(type, arguments, "=").value_or(TypeProperties::SpecialOperationState::Generated);
        const auto move =
            DeclaredSpecialOperation(type, arguments, "<-").value_or(TypeProperties::SpecialOperationState::Generated);
        return TypeProperties::FromOperations(copy, move, DeclaresDestructor(type, arguments));
    }

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

    const bool directlyDroppable = DeclaresDestructor(type, arguments);
    const auto copy = DeclaredSpecialOperation(type, arguments, "=");
    const auto move = DeclaredSpecialOperation(type, arguments, "<-");
    result.droppable = result.droppable || directlyDroppable;
    if (copy) {
        result.copyOperation = *copy;
    }
    if (move) {
        result.moveOperation = *move;
    }
    else if ((directlyDroppable || copy == TypeProperties::SpecialOperationState::Prohibited) &&
             result.moveOperation == TypeProperties::SpecialOperationState::Unresolved) {
        // A generic or imported recursive owner can leave its structural result unresolved until a concrete element
        // type is available. Its canonical destructor or explicit copy prohibition still establishes that this is a
        // complete owning type, and an absent move declaration is generated by contract. A structurally prohibited
        // field or an explicit move prohibition remains authoritative because neither produces Unresolved here.
        result.moveOperation = TypeProperties::SpecialOperationState::Generated;
    }
    result = TypeProperties::FromOperations(result.copyOperation, result.moveOperation, result.droppable);

    activeTypes.erase(key);
    cache.emplace(key, result);
    return result;
}

TypeProperties TypePropertyClassifier::ClassifyStruct(const StructDecl &declaration,
                                                      const std::vector<TypeRef> &arguments) {
    if (arguments.size() != declaration.typeParams.size()) {
        return TypeProperties::Unresolved();
    }

    const Substitutions substitutions = BindArguments(TypeParameterNames(declaration.typeParams), arguments);
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

    const Substitutions substitutions = BindArguments(TypeParameterNames(declaration.typeParams), arguments);
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

bool TypePropertyClassifier::DeclaresDestructor(const TypeRef &type, const std::vector<TypeRef> &arguments) const {
    const std::string baseName = BaseTypeName(type.name);
    const auto typeMethods = methodsByType.find(baseName);
    if (typeMethods == methodsByType.end()) {
        return false;
    }
    const auto destructors = typeMethods->second.find("~" + baseName);
    if (destructors == typeMethods->second.end()) {
        return false;
    }

    std::vector<std::string> parameters;
    if (const auto structure = structs.find(baseName); structure != structs.end()) {
        parameters = TypeParameterNames(structure->second->typeParams);
    }
    else if (const auto enumeration = enums.find(baseName); enumeration != enums.end()) {
        parameters = TypeParameterNames(enumeration->second->typeParams);
    }
    const Substitutions substitutions = BindArguments(parameters, arguments);
    return std::ranges::any_of(destructors->second, [&](const FuncDecl *method) {
        if (!method->body || !method->typeParams.empty() || method->params.size() != 1 || method->returnType ||
            method->params[0].name != "self" || method->params[0].isMut || method->params[0].isVariadic ||
            method->params[0].defaultValue) {
            return false;
        }
        const TypeRef receiver = resolveType(*method->params[0].type, substitutions);
        if (receiver.IsUnknown()) {
            // Declaration indexing precedes full type-node resolution across package modules. The destructor's
            // canonical signature is diagnosed independently; its reserved name and basic shape are enough to keep
            // ownership classification stable until the receiver type is available.
            return true;
        }
        return receiver.kind == TypeRef::Kind::Reference && !receiver.inner.empty() && receiver.inner.front().isMut &&
               SameValueType(receiver.inner.front(), type);
    });
}

std::optional<TypeProperties::SpecialOperationState>
TypePropertyClassifier::DeclaredSpecialOperation(const TypeRef &type, const std::vector<TypeRef> &arguments,
                                                 const std::string_view name) const {
    const std::string baseName = BaseTypeName(type.name);
    const auto typeMethods = methodsByType.find(baseName);
    if (typeMethods == methodsByType.end()) {
        return std::nullopt;
    }
    const auto operations = typeMethods->second.find(std::string(name));
    if (operations == typeMethods->second.end()) {
        return std::nullopt;
    }

    std::vector<std::string> parameters;
    if (const auto structure = structs.find(baseName); structure != structs.end()) {
        parameters = TypeParameterNames(structure->second->typeParams);
    }
    else if (const auto enumeration = enums.find(baseName); enumeration != enums.end()) {
        parameters = TypeParameterNames(enumeration->second->typeParams);
    }
    const Substitutions substitutions = BindArguments(parameters, arguments);
    for (const FuncDecl *method : operations->second) {
        if (IsCanonicalSpecialOperation(*method, type, substitutions, name == "=")) {
            return method->body ? TypeProperties::SpecialOperationState::Custom
                                : TypeProperties::SpecialOperationState::Prohibited;
        }
    }
    return std::nullopt;
}

bool TypePropertyClassifier::IsCanonicalSpecialOperation(const FuncDecl &method, const TypeRef &type,
                                                         const Substitutions &substitutions, const bool copy) const {
    if (!method.typeParams.empty() || method.params.size() != 2 || method.returnType ||
        method.params[0].name != "self" || method.params[1].name != "other" || method.params[0].isMut ||
        method.params[1].isMut || method.params[0].isVariadic || method.params[1].isVariadic ||
        method.params[0].defaultValue || method.params[1].defaultValue) {
        return false;
    }

    const TypeRef receiver = resolveType(*method.params[0].type, substitutions);
    const TypeRef other = resolveType(*method.params[1].type, substitutions);
    if (receiver.IsUnknown() || other.IsUnknown()) {
        // Special-operation validation reports malformed declarations separately. During early cross-module
        // classification, unresolved type nodes still carry the reserved operator name and canonical parameter
        // shape, which is enough to preserve the declared generated/custom/prohibited state.
        return true;
    }
    if (receiver.kind != TypeRef::Kind::Reference || receiver.inner.empty() || !receiver.inner.front().isMut ||
        !SameValueType(receiver.inner.front(), type)) {
        return false;
    }
    if (!copy) {
        return other.kind != TypeRef::Kind::Reference && SameValueType(other, type);
    }
    return other.kind == TypeRef::Kind::Reference && !other.inner.empty() && !other.inner.front().isMut &&
           SameValueType(other.inner.front(), type);
}

std::string TypePropertyClassifier::BaseTypeName(const std::string &name) {
    const std::size_t arguments = name.find('<');
    return arguments == std::string::npos ? name : name.substr(0, arguments);
}

TypeProperties TypePropertyClassifier::Combine(TypeProperties aggregate, const TypeProperties member) {
    aggregate.droppable = aggregate.droppable || member.droppable;
    const auto combineOperation = [](const TypeProperties::SpecialOperationState left,
                                     const TypeProperties::SpecialOperationState right) {
        if (left == TypeProperties::SpecialOperationState::Prohibited ||
            right == TypeProperties::SpecialOperationState::Prohibited) {
            return TypeProperties::SpecialOperationState::Prohibited;
        }
        if (left == TypeProperties::SpecialOperationState::Unresolved ||
            right == TypeProperties::SpecialOperationState::Unresolved) {
            return TypeProperties::SpecialOperationState::Unresolved;
        }
        return TypeProperties::SpecialOperationState::Generated;
    };
    return TypeProperties::FromOperations(combineOperation(aggregate.copyOperation, member.copyOperation),
                                          combineOperation(aggregate.moveOperation, member.moveOperation),
                                          aggregate.droppable);
}

bool TypePropertyClassifier::SameValueType(TypeRef left, TypeRef right) {
    left.isMut = false;
    right.isMut = false;
    return left == right;
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
