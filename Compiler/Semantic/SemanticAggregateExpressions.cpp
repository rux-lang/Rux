#include "Semantic/Detail/SemanticAnalyzerContext.h"

#include <algorithm>
#include <format>

namespace Rux::SemanticDetail {
namespace {
[[nodiscard]] std::optional<TypeRef> BuiltinTypeFromName(const std::string &name) {
    if (name == "opaque") {
        return TypeRef::MakeOpaque();
    }
    if (name == "bool" || name == "bool8") {
        return TypeRef::MakeBool8();
    }
    if (name == "bool16") {
        return TypeRef::MakeBool16();
    }
    if (name == "bool32") {
        return TypeRef::MakeBool32();
    }
    if (name == "char" || name == "char32") {
        return TypeRef::MakeChar32();
    }
    if (name == "char8") {
        return TypeRef::MakeChar8();
    }
    if (name == "char16") {
        return TypeRef::MakeChar16();
    }
    if (name == "int8") {
        return TypeRef::MakeInt8();
    }
    if (name == "int16") {
        return TypeRef::MakeInt16();
    }
    if (name == "int32") {
        return TypeRef::MakeInt32();
    }
    if (name == "int64") {
        return TypeRef::MakeInt64();
    }
    if (name == "int") {
        return TypeRef::MakeInt();
    }
    if (name == "byte" || name == "uint8") {
        return TypeRef::MakeUInt8();
    }
    if (name == "uint16") {
        return TypeRef::MakeUInt16();
    }
    if (name == "uint32") {
        return TypeRef::MakeUInt32();
    }
    if (name == "uint64") {
        return TypeRef::MakeUInt64();
    }
    if (name == "uint") {
        return TypeRef::MakeUInt();
    }
    if (name == "float32") {
        return TypeRef::MakeFloat32();
    }
    if (name == "float64") {
        return TypeRef::MakeFloat64();
    }
    if (name == "float") {
        return TypeRef::MakeFloat();
    }
    return std::nullopt;
}
} // namespace

std::string SemanticAnalyzerContext::NamedBaseTypeName(const TypeRef &type) const {
    const TypeRef *named = &type;
    if (type.kind == TypeRef::Kind::Pointer && !type.inner.empty()) {
        named = &type.inner[0];
    }
    if (named->kind == TypeRef::Kind::Named) {
        // Slice extension methods are keyed on the full element-specific name
        // (e.g. `Slice<int>`) so `extend int[]` stays distinct from `extend
        // str[]`; other named types collapse to their base name so generic
        // instantiations share one method set.
        if (named->name.starts_with("Slice<")) {
            return named->name;
        }
        return BaseTypeName(named->name);
    }
    switch (named->kind) {
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
        return named->ToString();
    default:
        return {};
    }
}

std::optional<TypeRef> SemanticAnalyzerContext::SliceElementType(const TypeRef &type) {
    if (type.kind != TypeRef::Kind::Named) {
        return std::nullopt;
    }
    constexpr std::string_view prefix = "Slice<";
    if (!type.name.starts_with(prefix) || type.name.back() != '>') {
        return std::nullopt;
    }
    const std::string elementName = type.name.substr(prefix.size(), type.name.size() - prefix.size() - 1);
    if (auto builtin = BuiltinTypeFromName(elementName)) {
        return builtin;
    }
    return TypeRef::MakeNamed(elementName);
}

std::optional<TypeRef> SemanticAnalyzerContext::IndexElementType(const TypeRef &type) {
    if (type.kind == TypeRef::Kind::Array && !type.inner.empty()) {
        return type.inner[0];
    }
    if (auto elementType = SliceElementType(type)) {
        return elementType;
    }
    if (type.kind == TypeRef::Kind::Pointer && !type.inner.empty()) {
        return type.inner[0];
    }
    return std::nullopt;
}

std::string SemanticAnalyzerContext::GenericStructInitName(const StructInitExpr &expression) {
    std::string name = expression.typeName;
    if (!expression.typeArgs.empty()) {
        name += "<";
        for (std::size_t i = 0; i < expression.typeArgs.size(); ++i) {
            if (i) {
                name += ", ";
            }
            name += ResolveType(*expression.typeArgs[i]).ToString();
        }
        name += ">";
    }
    return name;
}

std::pair<const EnumDecl *, const EnumDecl::Variant *>
SemanticAnalyzerContext::LookupEnumVariantInitializer(const std::string &typeName) const {
    const std::size_t separator = typeName.find("::");
    if (separator == std::string::npos || typeName.find("::", separator + 2) != std::string::npos) {
        return {nullptr, nullptr};
    }

    const std::string enumName = typeName.substr(0, separator);
    const std::string variantName = typeName.substr(separator + 2);
    const auto enumeration = enumDecls.find(enumName);
    if (enumeration == enumDecls.end()) {
        return {nullptr, nullptr};
    }
    for (const auto &variant : enumeration->second->variants) {
        if (variant.name == variantName) {
            return {enumeration->second, &variant};
        }
    }
    return {enumeration->second, nullptr};
}

std::unordered_map<std::string, TypeRef>
SemanticAnalyzerContext::StructTypeSubstitutions(const StructDecl &declaration,
                                                 const std::vector<TypeExprPtr> &typeArguments) {
    std::unordered_map<std::string, TypeRef> substitutions;
    const std::size_t count = std::min(declaration.typeParams.size(), typeArguments.size());
    for (std::size_t i = 0; i < count; ++i) {
        substitutions.emplace(declaration.typeParams[i], ResolveType(*typeArguments[i]));
    }
    return substitutions;
}

TypeRef SemanticAnalyzerContext::StructFieldType(const TypeRef &objectType, const std::string &fieldName) {
    const std::string typeName = NamedBaseTypeName(objectType);
    if (typeName.empty()) {
        return TypeRef::MakeUnknown();
    }
    const auto structure = structDecls.find(typeName);
    if (structure == structDecls.end()) {
        return TypeRef::MakeUnknown();
    }

    std::unordered_map<std::string, TypeRef> substitutions;
    const std::vector<TypeRef> typeArguments = ParseTypeArgsFromTypeName(objectType.name);
    const auto &parameters = structure->second->typeParams;
    const std::size_t count = std::min(parameters.size(), typeArguments.size());
    for (std::size_t i = 0; i < count; ++i) {
        substitutions.emplace(parameters[i], typeArguments[i]);
    }

    for (const auto &field : structure->second->fields) {
        if (field.name == fieldName) {
            if (!substitutions.empty()) {
                return ResolveTypeWithSubstitution(*field.type, substitutions);
            }
            return ResolveType(*field.type);
        }
    }
    return TypeRef::MakeUnknown();
}

void SemanticAnalyzerContext::CheckStructInitExpression(const StructInitExpr &expression) {
    const auto structure = structDecls.find(expression.typeName);
    if (structure == structDecls.end()) {
        if (const auto [enumDecl, variant] = LookupEnumVariantInitializer(expression.typeName); enumDecl) {
            if (!variant) {
                EmitError(expression.location,
                          std::format("unknown enum variant '{}' in initializer", expression.typeName));
                for (const auto &field : expression.fields) {
                    CheckExpr(*field.value);
                }
                return;
            }
            if (variant->namedFields.empty()) {
                EmitError(expression.location,
                          std::format("enum variant '{}' has no named fields", expression.typeName));
                for (const auto &field : expression.fields) {
                    CheckExpr(*field.value);
                }
                return;
            }

            std::unordered_map<std::string, const EnumDecl::Variant::NamedField *> fields;
            for (const auto &field : variant->namedFields) {
                fields.emplace(field.name, &field);
            }

            std::unordered_set<std::string> initialized;
            for (const auto &field : expression.fields) {
                const TypeRef valueType = CheckExpr(*field.value);
                if (!initialized.insert(field.name).second) {
                    EmitError(field.location, std::format("duplicate field '{}' in initializer for '{}'", field.name,
                                                          expression.typeName));
                    continue;
                }

                const auto expectedField = fields.find(field.name);
                if (expectedField == fields.end()) {
                    EmitError(field.location, std::format("unknown field '{}' in initializer for '{}'", field.name,
                                                          expression.typeName));
                    continue;
                }

                const TypeRef fieldType = ResolveType(*expectedField->second->type);
                if (!valueType.IsUnknown() && !fieldType.IsUnknown() &&
                    !CanAssignExprTo(*field.value, valueType, fieldType)) {
                    EmitError(field.location, AssignmentErrorMessage(
                                                  *field.value, fieldType,
                                                  std::format("cannot assign '{}' to field '{}' of type '{}'",
                                                              valueType.ToString(), field.name, fieldType.ToString())));
                }
            }

            for (const auto &field : variant->namedFields) {
                if (!initialized.contains(field.name)) {
                    EmitError(expression.location, std::format("missing field '{}' in initializer for '{}'", field.name,
                                                               expression.typeName));
                }
            }
            return;
        }

        EmitError(expression.location, std::format("unknown type '{}' in struct initializer", expression.typeName));
        for (const auto &field : expression.fields) {
            CheckExpr(*field.value);
        }
        return;
    }

    const StructDecl &declaration = *structure->second;
    if (expression.typeArgs.size() != declaration.typeParams.size()) {
        EmitError(expression.location,
                  std::format("struct '{}' expects {} type argument(s), got {}", expression.typeName,
                              declaration.typeParams.size(), expression.typeArgs.size()));
    }

    const auto substitutions = StructTypeSubstitutions(declaration, expression.typeArgs);
    std::unordered_map<std::string, const StructDecl::Field *> fields;
    for (const auto &field : declaration.fields) {
        fields.emplace(field.name, &field);
    }

    std::unordered_set<std::string> initialized;
    for (const auto &field : expression.fields) {
        const TypeRef valueType = CheckExpr(*field.value);
        if (!initialized.insert(field.name).second) {
            EmitError(field.location,
                      std::format("duplicate field '{}' in initializer for '{}'", field.name, expression.typeName));
            continue;
        }

        const auto expectedField = fields.find(field.name);
        if (expectedField == fields.end()) {
            EmitError(field.location,
                      std::format("unknown field '{}' in initializer for '{}'", field.name, expression.typeName));
            continue;
        }

        const TypeRef fieldType = ResolveTypeWithSubstitution(*expectedField->second->type, substitutions);
        if (!valueType.IsUnknown() && !fieldType.IsUnknown() && !CanAssignExprTo(*field.value, valueType, fieldType)) {
            EmitError(field.location,
                      AssignmentErrorMessage(*field.value, fieldType,
                                             std::format("cannot assign '{}' to field '{}' of type '{}'",
                                                         valueType.ToString(), field.name, fieldType.ToString())));
        }
    }

    for (const auto &field : declaration.fields) {
        if (!initialized.contains(field.name)) {
            EmitError(expression.location,
                      std::format("missing field '{}' in initializer for '{}'", field.name, expression.typeName));
        }
    }
}

std::optional<TypeRef> SemanticAnalyzerContext::CheckAggregateExpression(const Expr &expression) {
    if (const auto *index = dynamic_cast<const IndexExpr *>(&expression)) {
        const TypeRef objectType = CheckExpr(*index->object);
        const TypeRef indexType = CheckExpr(*index->index);
        if (indexType.IsRange()) {
            std::optional<TypeRef> elementType;
            if (objectType.kind == TypeRef::Kind::Array && !objectType.inner.empty()) {
                elementType = objectType.inner[0];
            }
            else {
                elementType = SliceElementType(objectType);
            }
            if (elementType) {
                return TypeRef::MakeNamed(SliceTypeName(*elementType));
            }
            EmitError(index->location, std::format("cannot slice value of type '{}'", objectType.ToString()));
            return TypeRef::MakeUnknown();
        }
        if (auto elementType = IndexElementType(objectType)) {
            return *elementType;
        }
        return TypeRef::MakeUnknown();
    }

    if (const auto *field = dynamic_cast<const FieldExpr *>(&expression)) {
        const TypeRef objectType = CheckExpr(*field->object);
        if (objectType.kind == TypeRef::Kind::Array && objectType.arrayLength && field->field == "length") {
            return TypeRef::MakeUInt();
        }
        if (auto elementType = SliceElementType(objectType)) {
            if (field->field == "data") {
                return TypeRef::MakePointer(*elementType);
            }
            if (field->field == "length") {
                return TypeRef::MakeUInt64();
            }
            EmitError(field->location,
                      std::format("unknown field '{}' on type '{}'", field->field, objectType.ToString()));
            return TypeRef::MakeUnknown();
        }
        if (objectType.IsRange()) {
            const TypeRef elementType = objectType.inner.empty() ? TypeRef::MakeInt64() : objectType.inner[0];
            if (field->field == "start" && objectType.RangeHasStart()) {
                return elementType;
            }
            if (field->field == "end" && objectType.RangeHasEnd()) {
                return elementType;
            }
            EmitError(field->location,
                      std::format("unknown field '{}' on type '{}'", field->field, objectType.ToString()));
            return TypeRef::MakeUnknown();
        }
        if (objectType.kind == TypeRef::Kind::Tuple) {
            try {
                const std::size_t index = std::stoul(field->field);
                if (index < objectType.inner.size()) {
                    return objectType.inner[index];
                }
            }
            catch (...) {
            }
            EmitError(field->location,
                      std::format("tuple index '{}' out of range for type '{}'", field->field, objectType.ToString()));
            return TypeRef::MakeUnknown();
        }

        // Interface fat-pointer fields: data -> *opaque, vtable -> *opaque.
        if (const std::string interfaceName = NamedBaseTypeName(objectType);
            !interfaceName.empty() && currentScope->Lookup(interfaceName) &&
            currentScope->Lookup(interfaceName)->kind == Symbol::Kind::Interface) {
            const TypeRef opaquePointer = TypeRef::MakePointer(TypeRef::MakeOpaque());
            if (field->field == "data" || field->field == "vtable") {
                return opaquePointer;
            }
            EmitError(field->location,
                      std::format("unknown field '{}' on interface type '{}'", field->field, objectType.ToString()));
            return TypeRef::MakeUnknown();
        }

        const std::string structName = NamedBaseTypeName(objectType);
        if (!structName.empty() && structDecls.contains(structName)) {
            if (TypeRef fieldType = StructFieldType(objectType, field->field); !fieldType.IsUnknown()) {
                return fieldType;
            }
            EmitError(field->location,
                      std::format("unknown field '{}' on type '{}'", field->field, objectType.ToString()));
            return TypeRef::MakeUnknown();
        }

        if (TypeRef fieldType = StructFieldType(objectType, field->field); !fieldType.IsUnknown()) {
            return fieldType;
        }
        if (!objectType.IsUnknown()) {
            EmitError(field->location, std::format("type '{}' has no field '{}'", objectType.ToString(), field->field));
        }
        return TypeRef::MakeUnknown();
    }

    if (const auto *initializer = dynamic_cast<const StructInitExpr *>(&expression)) {
        CheckStructInitExpression(*initializer);
        if (const auto [enumDecl, variant] = LookupEnumVariantInitializer(initializer->typeName); enumDecl && variant) {
            return EnumType(*enumDecl);
        }
        const std::string typeName = GenericStructInitName(*initializer);
        TypeRef type = ParseTypeRefFromString(typeName);
        return type.IsRange() ? type : TypeRef::MakeNamed(typeName);
    }

    if (const auto *array = dynamic_cast<const ArrayExpr *>(&expression)) {
        TypeRef elementType = TypeRef::MakeUnknown();
        for (const auto &element : array->elements) {
            const TypeRef type = CheckExpr(*element);
            if (elementType.IsUnknown()) {
                elementType = type;
            }
        }
        return TypeRef::MakeArray(elementType, array->elements.size());
    }

    if (const auto *tuple = dynamic_cast<const TupleExpr *>(&expression)) {
        std::vector<TypeRef> elementTypes;
        for (const auto &element : tuple->elements) {
            elementTypes.push_back(CheckExpr(*element));
        }
        return TypeRef::MakeTuple(std::move(elementTypes));
    }

    return std::nullopt;
}
} // namespace Rux::SemanticDetail
