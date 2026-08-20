// Checking for struct, enum, union, array, slice and index expressions.

#include "Semantic/Detail/SemanticAnalyzerContext.h"

#include <algorithm>
#include <format>

namespace Rux::SemanticDetail {
namespace {
template <typename Range, typename Projection>
[[nodiscard]] std::string AvailableNames(const Range &values, Projection projection) {
    std::vector<std::string> names;
    names.reserve(values.size());
    for (const auto &value : values) {
        names.push_back(std::format("'{}'", projection(value)));
    }
    std::ranges::sort(names);
    if (names.empty()) {
        return "none";
    }
    std::string result;
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (index != 0) {
            result += index + 1 == names.size() ? " and " : ", ";
        }
        result += names[index];
    }
    return result;
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
    // Every primitive names its own method set, so a width added to the catalog joins without a case here.
    if (named->IsPrimitive() || named->kind == TypeRef::Kind::Str) {
        return named->ToString();
    }
    return {};
}

std::optional<TypeRef> SemanticAnalyzerContext::SliceElementType(const TypeRef &type) const {
    if (type.kind != TypeRef::Kind::Named) {
        return std::nullopt;
    }
    constexpr std::string_view prefix = "Slice<";
    if (!type.name.starts_with(prefix) || type.name.back() != '>') {
        return std::nullopt;
    }
    // One reader for a type written inside a name, so an element is the same type here as anywhere else it is read
    // back: a type parameter in scope stays a parameter, and a pointer or tuple element is not flattened to a name
    // that merely looks like one.
    return ParseTypeRefFromString(type.name.substr(prefix.size(), type.name.size() - prefix.size() - 1));
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
    const EnumDecl *enumeration = EnumNamed(enumName);
    if (!enumeration) {
        return {nullptr, nullptr};
    }
    for (const auto &variant : enumeration->variants) {
        if (variant.name == variantName) {
            return {enumeration, &variant};
        }
    }
    return {enumeration, nullptr};
}

std::unordered_map<std::string, TypeRef>
SemanticAnalyzerContext::StructTypeSubstitutions(const StructDecl &declaration,
                                                 const std::vector<TypeExprPtr> &typeArguments) {
    std::unordered_map<std::string, TypeRef> substitutions;
    const std::size_t count = std::min(declaration.typeParams.size(), typeArguments.size());
    for (std::size_t i = 0; i < count; ++i) {
        substitutions.emplace(declaration.typeParams[i].name, ResolveType(*typeArguments[i]));
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
        if (const auto unionType = unionDecls.find(typeName); unionType != unionDecls.end()) {
            for (const auto &field : unionType->second->fields) {
                if (field.name == fieldName) {
                    return ResolveType(*field.type);
                }
            }
        }
        return TypeRef::MakeUnknown();
    }

    // The arguments are spelled in the name of the type that carries them, which is the pointee when the field is
    // reached through a pointer -- a pointer has no name of its own, so reading them off `objectType` would leave a
    // generic instantiation looking like a bare declaration and its fields still spelled in the type parameters.
    const TypeRef &named =
        objectType.kind == TypeRef::Kind::Pointer && !objectType.inner.empty() ? objectType.inner[0] : objectType;
    std::unordered_map<std::string, TypeRef> substitutions;
    const std::vector<TypeRef> typeArguments = ParseTypeArgsFromTypeName(named.name);
    const auto &parameters = structure->second->typeParams;
    const std::size_t count = std::min(parameters.size(), typeArguments.size());
    for (std::size_t i = 0; i < count; ++i) {
        substitutions.emplace(parameters[i].name, typeArguments[i]);
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
        if (const auto unionType = unionDecls.find(expression.typeName); unionType != unionDecls.end()) {
            if (!expression.typeArgs.empty()) {
                EmitError(expression.location, std::format("union initializer for '{}' does not accept type arguments",
                                                           expression.typeName));
            }
            if (expression.fields.size() != 1) {
                EmitError(expression.location,
                          std::format("union initializer for '{}' must select exactly one field, but {} provided",
                                      expression.typeName,
                                      expression.fields.size() == 1
                                          ? "1 was"
                                          : std::format("{} were", expression.fields.size())));
            }
            std::unordered_set<std::string> seen;
            for (const auto &field : expression.fields) {
                const TypeRef valueType = CheckExpr(*field.value);
                if (!seen.insert(field.name).second) {
                    EmitError(field.location, std::format("field '{}' is initialized more than once for union '{}'",
                                                          field.name, expression.typeName));
                    continue;
                }
                const auto expectedField =
                    std::ranges::find_if(unionType->second->fields, [&](const UnionDecl::Field &candidate) {
                        return candidate.name == field.name;
                    });
                if (expectedField == unionType->second->fields.end()) {
                    EmitError(
                        field.location, std::format("union '{}' has no field '{}'", expression.typeName, field.name),
                        {std::format("available fields are {}",
                                     AvailableNames(unionType->second->fields, [](const UnionDecl::Field &candidate) {
                                         return candidate.name;
                                     }))});
                    continue;
                }
                const TypeRef fieldType = ResolveType(*expectedField->type);
                if (!valueType.IsUnknown() && !fieldType.IsUnknown() &&
                    !CanAssignExprTo(*field.value, valueType, fieldType)) {
                    EmitError(
                        field.location,
                        AssignmentErrorMessage(
                            *field.value, fieldType,
                            std::format("field '{}' in initializer for union '{}' has type '{}', but its "
                                        "declaration requires '{}'",
                                        field.name, expression.typeName, valueType.ToString(), fieldType.ToString())),
                        {std::format("field '{}' declared at line {}, column {}", field.name,
                                     expectedField->location.line, expectedField->location.column)});
                }
                else if (!valueType.IsUnknown() && !fieldType.IsUnknown()) {
                    ConsumeValue(*field.value, valueType, ValueConsumptionKind::Aggregate, field.location);
                }
            }
            return;
        }

        if (const auto [enumDecl, variant] = LookupEnumVariantInitializer(expression.typeName); enumDecl) {
            if (!variant) {
                EmitError(expression.location,
                          std::format("enum '{}' has no variant '{}'", enumDecl->name,
                                      expression.typeName.substr(expression.typeName.find("::") + 2)),
                          {std::format("available variants are {}",
                                       AvailableNames(enumDecl->variants, [](const EnumDecl::Variant &candidate) {
                                           return candidate.name;
                                       }))});
                for (const auto &field : expression.fields) {
                    CheckExpr(*field.value);
                }
                return;
            }
            if (variant->namedFields.empty()) {
                EmitError(expression.location,
                          std::format("enum variant '{}' cannot use a named-field initializer because it has no named "
                                      "fields",
                                      expression.typeName));
                for (const auto &field : expression.fields) {
                    CheckExpr(*field.value);
                }
                return;
            }

            std::unordered_map<std::string, const EnumDecl::Variant::NamedField *> fields;
            for (const auto &field : variant->namedFields) {
                fields.emplace(field.name, &field);
            }

            std::unordered_map<std::string, SourceLocation> initialized;
            for (const auto &field : expression.fields) {
                const TypeRef valueType = CheckExpr(*field.value);
                if (const auto [first, inserted] = initialized.emplace(field.name, field.location); !inserted) {
                    EmitError(field.location,
                              std::format("field '{}' is initialized more than once for '{}'", field.name,
                                          expression.typeName),
                              {std::format("the first initializer for '{}' is at line {}, column {}", field.name,
                                           first->second.line, first->second.column)});
                    continue;
                }

                const auto expectedField = fields.find(field.name);
                if (expectedField == fields.end()) {
                    EmitError(field.location,
                              std::format("enum variant '{}' has no field '{}'", expression.typeName, field.name),
                              {std::format("available fields are {}",
                                           AvailableNames(variant->namedFields,
                                                          [](const auto &candidate) { return candidate.name; }))});
                    continue;
                }

                const TypeRef fieldType = ResolveType(*expectedField->second->type);
                if (!valueType.IsUnknown() && !fieldType.IsUnknown() &&
                    !CanAssignExprTo(*field.value, valueType, fieldType)) {
                    EmitError(
                        field.location,
                        AssignmentErrorMessage(*field.value, fieldType,
                                               std::format("field '{}' in initializer for '{}' has type '{}', but its "
                                                           "declaration requires '{}'",
                                                           field.name, expression.typeName, valueType.ToString(),
                                                           fieldType.ToString())),
                        {std::format("field '{}' declared at line {}, column {}", field.name,
                                     expectedField->second->location.line, expectedField->second->location.column)});
                }
                else if (!valueType.IsUnknown() && !fieldType.IsUnknown()) {
                    ConsumeValue(*field.value, valueType, ValueConsumptionKind::Aggregate, field.location);
                }
            }

            for (const auto &field : variant->namedFields) {
                if (!initialized.contains(field.name)) {
                    EmitError(expression.location,
                              std::format("initializer for '{}' is missing required field '{}'", expression.typeName,
                                          field.name),
                              {std::format("field '{}' declared at line {}, column {}", field.name, field.location.line,
                                           field.location.column)});
                }
            }
            return;
        }

        EmitError(expression.location,
                  std::format("type '{}' is not defined for an aggregate initializer", expression.typeName));
        for (const auto &field : expression.fields) {
            CheckExpr(*field.value);
        }
        return;
    }

    const StructDecl &declaration = *structure->second;
    if (expression.typeArgs.size() != declaration.typeParams.size()) {
        EmitError(expression.location,
                  std::format(
                      "struct initializer for '{}' requires {} type argument{}, but {} provided", expression.typeName,
                      declaration.typeParams.size(), declaration.typeParams.size() == 1 ? "" : "s",
                      expression.typeArgs.size() == 1 ? "1 was" : std::format("{} were", expression.typeArgs.size())));
    }

    const auto substitutions = StructTypeSubstitutions(declaration, expression.typeArgs);
    CheckTypeArgumentConstraints(declaration.typeParams, substitutions, expression.location,
                                 std::format("struct '{}'", expression.typeName));
    std::unordered_map<std::string, const StructDecl::Field *> fields;
    for (const auto &field : declaration.fields) {
        fields.emplace(field.name, &field);
    }

    std::unordered_map<std::string, SourceLocation> initialized;
    for (const auto &field : expression.fields) {
        const TypeRef valueType = CheckExpr(*field.value);
        if (const auto [first, inserted] = initialized.emplace(field.name, field.location); !inserted) {
            EmitError(field.location,
                      std::format("field '{}' is initialized more than once for '{}'", field.name, expression.typeName),
                      {std::format("the first initializer for '{}' is at line {}, column {}", field.name,
                                   first->second.line, first->second.column)});
            continue;
        }

        const auto expectedField = fields.find(field.name);
        if (expectedField == fields.end()) {
            EmitError(field.location, std::format("struct '{}' has no field '{}'", expression.typeName, field.name),
                      {std::format("available fields are {}",
                                   AvailableNames(declaration.fields,
                                                  [](const StructDecl::Field &candidate) { return candidate.name; }))});
            continue;
        }

        const TypeRef fieldType = ResolveTypeWithSubstitution(*expectedField->second->type, substitutions);
        if (!valueType.IsUnknown() && !fieldType.IsUnknown() && !CanAssignExprTo(*field.value, valueType, fieldType)) {
            EmitError(field.location,
                      AssignmentErrorMessage(
                          *field.value, fieldType,
                          std::format("field '{}' in initializer for '{}' has type '{}', but its declaration requires "
                                      "'{}'",
                                      field.name, expression.typeName, valueType.ToString(), fieldType.ToString())),
                      {std::format("field '{}' declared at line {}, column {}", field.name,
                                   expectedField->second->location.line, expectedField->second->location.column)});
        }
        else if (!valueType.IsUnknown() && !fieldType.IsUnknown()) {
            ConsumeValue(*field.value, valueType, ValueConsumptionKind::Aggregate, field.location);
        }
    }

    for (const auto &field : declaration.fields) {
        if (!initialized.contains(field.name)) {
            EmitError(
                expression.location,
                std::format("initializer for '{}' is missing required field '{}'", expression.typeName, field.name),
                {std::format("field '{}' declared at line {}, column {}", field.name, field.location.line,
                             field.location.column)});
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
        if (!indexType.IsUnknown() && !indexType.IsInteger()) {
            EmitError(index->index->location,
                      std::format("index for type '{}' must be an integer or range, but has type '{}'",
                                  objectType.ToString(), indexType.ToString()));
            return TypeRef::MakeUnknown();
        }
        if (auto elementType = IndexElementType(objectType)) {
            return *elementType;
        }
        if (!objectType.IsUnknown()) {
            EmitError(index->object->location, std::format("type '{}' cannot be indexed", objectType.ToString()), {},
                      "only arrays, slices, and pointers support indexing");
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
                      std::format("slice type '{}' has no member '{}'", objectType.ToString(), field->field),
                      {"available slice members are 'data' and 'length'"});
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
                      std::format("range type '{}' has no member '{}'", objectType.ToString(), field->field),
                      {"range members are 'start' and 'end' when that bound is present"});
            return TypeRef::MakeUnknown();
        }
        if (objectType.kind == TypeRef::Kind::Tuple) {
            try {
                std::size_t consumed = 0;
                const std::size_t index = std::stoul(field->field, &consumed);
                if (consumed == field->field.size()) {
                    if (index < objectType.inner.size()) {
                        return objectType.inner[index];
                    }
                    EmitError(field->location,
                              std::format("tuple index {} is out of range for a tuple with {} element{}", index,
                                          objectType.inner.size(), objectType.inner.size() == 1 ? "" : "s"));
                    return TypeRef::MakeUnknown();
                }
            }
            catch (...) {
            }
            EmitError(field->location,
                      std::format("tuple type '{}' has no member '{}'", objectType.ToString(), field->field), {},
                      "tuple members use zero-based numeric indices such as '.0'");
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
                      std::format("interface type '{}' has no member '{}'", objectType.ToString(), field->field),
                      {"available interface representation members are 'data' and 'vtable'"});
            return TypeRef::MakeUnknown();
        }

        const std::string structName = NamedBaseTypeName(objectType);
        if (!structName.empty() && structDecls.contains(structName)) {
            if (TypeRef fieldType = StructFieldType(objectType, field->field); !fieldType.IsUnknown()) {
                return fieldType;
            }
            EmitError(field->location,
                      std::format("struct '{}' has no field '{}'", objectType.ToString(), field->field),
                      {std::format("available fields are {}",
                                   AvailableNames(structDecls.at(structName)->fields,
                                                  [](const StructDecl::Field &candidate) { return candidate.name; }))});
            return TypeRef::MakeUnknown();
        }
        if (!structName.empty() && unionDecls.contains(structName)) {
            if (TypeRef fieldType = StructFieldType(objectType, field->field); !fieldType.IsUnknown()) {
                return fieldType;
            }
            EmitError(field->location, std::format("union '{}' has no field '{}'", objectType.ToString(), field->field),
                      {std::format("available fields are {}",
                                   AvailableNames(unionDecls.at(structName)->fields,
                                                  [](const UnionDecl::Field &candidate) { return candidate.name; }))});
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
        std::size_t inferredFrom = 0;
        const Expr *inferredExpression = nullptr;
        for (std::size_t index = 0; index < array->elements.size(); ++index) {
            const auto &element = array->elements[index];
            const TypeRef type = CheckExpr(*element);
            bool elementAccepted = !type.IsUnknown();
            if (elementType.IsUnknown()) {
                elementType = type;
                inferredFrom = index;
                inferredExpression = element.get();
            }
            else if (!type.IsUnknown() && !CanAssignExprTo(*element, type, elementType)) {
                if (inferredExpression && CanAssignExprTo(*inferredExpression, elementType, type)) {
                    elementType = type;
                    inferredFrom = index;
                    inferredExpression = element.get();
                }
                else {
                    elementAccepted = false;
                    EmitError(
                        element->location,
                        std::format("array element {} has type '{}', but element {} established element type '{}'",
                                    index + 1, type.ToString(), inferredFrom + 1, elementType.ToString()));
                }
            }
            if (elementAccepted) {
                ConsumeValue(*element, type, ValueConsumptionKind::Aggregate, element->location);
            }
        }
        return TypeRef::MakeArray(elementType, array->elements.size());
    }

    if (const auto *tuple = dynamic_cast<const TupleExpr *>(&expression)) {
        std::vector<TypeRef> elementTypes;
        for (const auto &element : tuple->elements) {
            const TypeRef type = CheckExpr(*element);
            ConsumeValue(*element, type, ValueConsumptionKind::Aggregate, element->location);
            elementTypes.push_back(type);
        }
        return TypeRef::MakeTuple(std::move(elementTypes));
    }

    return std::nullopt;
}
} // namespace Rux::SemanticDetail
