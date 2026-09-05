// Checking for struct, enum, union, array, slice and index expressions.

#include "Semantic/Analysis/AnalysisContext.h"
#include "Types/PrimitiveCatalog.h"

#include <algorithm>
#include <format>

namespace Rux::SemanticDetail {
namespace {
[[nodiscard]] const TypeRef &ReferencedValue(const TypeRef &type) {
    return type.kind == TypeRef::Kind::Reference && !type.inner.empty() ? type.inner.front() : type;
}

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

template <typename Range, typename Projection, typename Predicate>
[[nodiscard]] std::string AvailableNamesIf(const Range &values, Projection projection, Predicate predicate) {
    std::vector<std::string> names;
    for (const auto &value : values) {
        if (predicate(value)) {
            names.push_back(std::format("'{}'", projection(value)));
        }
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

bool AnalysisContext::IsIndexOperatorCall(const Expr &expression) const {
    const auto *index = dynamic_cast<const IndexExpr *>(&expression);
    return index && (indexOperators.contains(index) || indexAssignments.contains(index));
}

std::optional<TypeRef> AnalysisContext::ResolveIndexAssignment(const IndexExpr &index, const TypeRef &objectType,
                                                               const TypeRef &indexType) {
    const std::vector<const FuncDecl *> candidates = AccessibleMethodCandidates(objectType, "[]=");
    if (candidates.empty()) {
        return std::nullopt;
    }

    // From here the receiver does declare the operator, so every remaining outcome is reported against it rather than
    // falling back to the read operator's diagnostics, which would name the wrong problem. A malformed declaration was
    // already reported where it was written, so it is skipped here rather than reported a second time per use.
    std::vector<const FuncDecl *> wellFormed;
    std::vector<std::vector<TypeRef>> wellFormedParameters;
    for (const FuncDecl *candidate : candidates) {
        std::vector<TypeRef> parameterTypes = ResolveOperatorParameterTypes(objectType, *candidate);
        if (parameterTypes.size() == 2 && !candidate->returnType) {
            wellFormed.push_back(candidate);
            wellFormedParameters.push_back(std::move(parameterTypes));
        }
    }
    if (wellFormed.empty()) {
        return TypeRef::MakeUnknown();
    }

    // Overloads are separated by their index type, exactly as the read operator's are.
    std::vector<std::size_t> matched;
    for (std::size_t candidate = 0; candidate < wellFormed.size(); ++candidate) {
        const TypeRef &declaredIndex = wellFormedParameters[candidate][0];
        if (declaredIndex.IsUnknown() || indexType.IsUnknown() ||
            CanAssignExprTo(*index.index, indexType, declaredIndex)) {
            matched.push_back(candidate);
        }
    }
    if (matched.empty()) {
        EmitError(index.index->location, std::format("no '[]=' on '{}' accepts an index of type '{}'",
                                                     objectType.ToString(), indexType.ToString()));
        return TypeRef::MakeUnknown();
    }
    if (matched.size() > 1) {
        EmitError(index.location,
                  std::format("index of type '{}' matches {} '[]=' overloads on '{}'", indexType.ToString(),
                              matched.size(), objectType.ToString()),
                  {"overloads of an indexed assignment are separated by their index type"});
        return TypeRef::MakeUnknown();
    }

    const FuncDecl *method = wellFormed[matched.front()];
    const TypeRef &valueType = wellFormedParameters[matched.front()][1];
    indexAssignments.insert_or_assign(&index, ResolvedIndexAssignment{method, objectType, indexType, valueType});
    return valueType;
}

void AnalysisContext::FinishIndexedAssignment(const AssignExpr &assignment, const TypeRef &valueParameterType,
                                              const TypeRef &valueType) {
    if (valueParameterType.IsUnknown() || valueType.IsUnknown()) {
        return;
    }
    if (!CanAssignExprTo(*assignment.value, valueType, valueParameterType)) {
        EmitError(assignment.value->location,
                  AssignmentErrorMessage(*assignment.value, valueParameterType,
                                         std::format("cannot pass '{}' to parameter of type '{}'", valueType.ToString(),
                                                     valueParameterType.ToString())));
        return;
    }
    // The new value reaches the setter as an ordinary by-value argument, so it transfers under the argument rules
    // rather than the rules for overwriting a place: nothing here is being replaced.
    if (assignment.op == TokenKind::MoveArrow) {
        ConsumeExplicitValue(*assignment.value, valueType, assignment.location);
    }
    else {
        ConsumeValue(*assignment.value, valueType, ValueConsumptionKind::Argument, assignment.location);
    }
}

MovePlace AnalysisContext::AnalyzeMovePlace(const Expr &expression) const {
    return SemanticDetail::AnalyzeMovePlace(expression,
                                            [this](const IndexExpr &index) { return indexOperators.contains(&index); });
}

bool AnalysisContext::SameStoragePlace(const Expr &left, const Expr &right) const {
    return SemanticDetail::SameStoragePlace(left, right,
                                            [this](const IndexExpr &index) { return indexOperators.contains(&index); });
}

const IndexExpr *AnalysisContext::IndexOperatorInPlace(const Expr &place) const {
    const Expr *root = &place;
    while (true) {
        if (const auto *field = dynamic_cast<const FieldExpr *>(root)) {
            root = field->object.get();
            continue;
        }
        if (const auto *index = dynamic_cast<const IndexExpr *>(root)) {
            if (indexOperators.contains(index)) {
                return index;
            }
            root = index->object.get();
            continue;
        }
        return nullptr;
    }
}

std::string AnalysisContext::NamedBaseTypeName(const TypeRef &type) const {
    const TypeRef *named = &type;
    if ((type.kind == TypeRef::Kind::Pointer || type.kind == TypeRef::Kind::Reference) && !type.inner.empty()) {
        named = &type.inner[0];
    }
    if (named->kind == TypeRef::Kind::Named) {
        // Slice extension methods are keyed on the full element-specific name
        // (e.g. `Slice<int>`) so `extend int[]` stays distinct from `extend
        // str[]`; other named types collapse to their base name so generic
        // instantiations share one method set.
        if (named->isIntrinsicSlice) {
            return named->name;
        }
        return BaseTypeName(named->name);
    }
    // Every primitive names its own method set, so a width added to the catalog joins without a case here.
    if (named->IsPrimitive()) {
        return named->ToString();
    }
    return {};
}

std::optional<TypeRef> AnalysisContext::SliceElementType(const TypeRef &type) const {
    if (!type.isIntrinsicSlice) {
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

std::optional<TypeRef> AnalysisContext::IndexElementType(const TypeRef &type) {
    const TypeRef &value = ReferencedValue(type);
    if (value.kind == TypeRef::Kind::Array && !value.inner.empty()) {
        return value.inner[0];
    }
    if (auto elementType = SliceElementType(value)) {
        return elementType;
    }
    // A string is indexed in the code units of its own encoding, which is what its length counts.
    if (value.IsString()) {
        return TypeRef::MakePrimitive(StringCodeUnitKind(value.kind));
    }
    if (value.kind == TypeRef::Kind::Pointer && !value.inner.empty()) {
        return value.inner[0];
    }
    return std::nullopt;
}

std::string AnalysisContext::GenericStructInitName(const StructInitExpr &expression) {
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
AnalysisContext::LookupEnumVariantInitializer(const std::string &typeName) const {
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
AnalysisContext::StructTypeSubstitutions(const StructDecl &declaration, const std::vector<TypeExprPtr> &typeArguments) {
    std::unordered_map<std::string, TypeRef> substitutions;
    const std::size_t count = std::min(declaration.typeParams.size(), typeArguments.size());
    for (std::size_t i = 0; i < count; ++i) {
        substitutions.emplace(declaration.typeParams[i].name, ResolveType(*typeArguments[i]));
    }
    return substitutions;
}

TypeRef AnalysisContext::StructFieldType(const TypeRef &objectType, const std::string &fieldName) {
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
        ((objectType.kind == TypeRef::Kind::Pointer || objectType.kind == TypeRef::Kind::Reference) &&
         !objectType.inner.empty())
            ? objectType.inner[0]
            : objectType;
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

void AnalysisContext::CheckStructInitExpression(const StructInitExpr &expression) {
    if (const auto type = PrimitiveTypeFromName(expression.typeName); type && type->IsString()) {
        EmitError(expression.location, "a string view cannot be constructed from raw fields",
                  {"string literals provide validated text; raw code units belong in a slice"});
        return;
    }
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
                    EmitError(field.location,
                              std::format("union '{}' has no field '{}'", expression.typeName, field.name),
                              {std::format("available fields are {}",
                                           AvailableNamesIf(
                                               unionType->second->fields,
                                               [](const UnionDecl::Field &candidate) { return candidate.name; },
                                               [this, &unionType](const UnionDecl::Field &candidate) {
                                                   return IsMemberAccessible(*unionType->second, candidate.isPublic);
                                               }))});
                    continue;
                }
                if (!IsMemberAccessible(*unionType->second, expectedField->isPublic)) {
                    EmitPrivateMemberError(field.location, *unionType->second, "union field", field.name);
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
                          std::format("variant '{}' has no case '{}'", enumDecl->name,
                                      expression.typeName.substr(expression.typeName.find("::") + 2)),
                          {std::format("available cases are {}",
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
                          std::format("variant case '{}' cannot use a named-field initializer because it has no named "
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
                              std::format("variant case '{}' has no field '{}'", expression.typeName, field.name),
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
            EmitError(
                field.location, std::format("struct '{}' has no field '{}'", expression.typeName, field.name),
                {std::format("available fields are {}",
                             AvailableNamesIf(
                                 declaration.fields, [](const StructDecl::Field &candidate) { return candidate.name; },
                                 [this, &declaration](const StructDecl::Field &candidate) {
                                     return IsMemberAccessible(declaration, candidate.isPublic);
                                 }))});
            continue;
        }

        if (!IsMemberAccessible(declaration, expectedField->second->isPublic)) {
            EmitPrivateMemberError(field.location, declaration, "struct field", field.name);
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

    bool hasInaccessibleField = false;
    for (const auto &field : declaration.fields) {
        if (!IsMemberAccessible(declaration, field.isPublic)) {
            hasInaccessibleField = true;
            continue;
        }
        if (!initialized.contains(field.name)) {
            EmitError(
                expression.location,
                std::format("initializer for '{}' is missing required field '{}'", expression.typeName, field.name),
                {std::format("field '{}' declared at line {}, column {}", field.name, field.location.line,
                             field.location.column)});
        }
    }
    if (hasInaccessibleField) {
        EmitError(expression.location,
                  std::format("struct '{}' cannot be initialized outside its package because it has private fields",
                              expression.typeName),
                  {}, "use a public constructor instead");
    }
}

std::optional<TypeRef> AnalysisContext::CheckAggregateExpression(const Expr &expression) {
    if (const auto *index = dynamic_cast<const IndexExpr *>(&expression)) {
        const bool wasProjectionRoot = checkingBorrowProjectionRoot;
        checkingBorrowProjectionRoot = true;
        const TypeRef objectType = CheckExpr(*index->object);
        checkingBorrowProjectionRoot = wasProjectionRoot;
        const TypeRef &objectValueType = ReferencedValue(objectType);
        const TypeRef indexType = CheckExpr(*index->index);
        const std::optional<TypeRef> builtinElementType = IndexElementType(objectType);
        const bool readsPlace = !wasProjectionRoot && !checkingPlainAssignmentTarget;

        // Written to rather than read, this index is an assignment through `[]=`, and the value it must accept comes
        // from that operator rather than from `[]`. Only the assignment's own target takes this route, so a subscript
        // nested inside one is still read.
        if (!builtinElementType && !objectType.IsUnknown() && index == indexAssignmentTarget) {
            if (std::optional<TypeRef> assigned = ResolveIndexAssignment(*index, objectType, indexType)) {
                // The setter declares `self: &var T`, so the receiver has to be writable, and it is mutated for the
                // duration of one call. Writability is the same question assigning into a built-in element asks: a
                // reference or pointer carries the permission in its own type, and anything else is as writable as
                // the place it names.
                if ((objectType.kind == TypeRef::Kind::Pointer || objectType.kind == TypeRef::Kind::Reference) &&
                    !objectType.inner.empty()) {
                    if (!objectType.inner.front().isMut) {
                        EmitError(index->location,
                                  objectType.kind == TypeRef::Kind::Reference
                                      ? std::format("cannot modify data through immutable reference '{}'",
                                                    objectType.ToString())
                                      : std::format("cannot modify data through read-only pointer '{}'",
                                                    objectType.ToString()));
                    }
                }
                else {
                    CheckMutability(*index->object);
                }
                CheckBorrowedMutation(*index->object, index->location);
                return *assigned;
            }
        }

        // A type the language does not index itself may declare `[]` in an `extend` block. The operator is tried after
        // the built-in forms, so indexing an array, slice, or pointer never depends on what an `extend` block declares,
        // and before the range and integer rules below, so an overload may accept whichever index type its author
        // chose. Overloads are separated by that index type, which is what lets `v[i]` and `v[1..4]` coexist.
        if (!builtinElementType && !objectType.IsUnknown() && !indexType.IsUnknown()) {
            if (const FuncDecl *method = LookupOperatorMethod(objectType, "[]", {indexType})) {
                // The object is a call receiver here, not a place the expression projects into, so the whole object
                // is what this reads and an exclusive borrow of any part of it conflicts.
                if (readsPlace) {
                    CheckBorrowedPlaceRead(*index->object, index->object->location);
                }
                const std::vector<TypeRef> parameterTypes = ResolveOperatorParameterTypes(objectType, *method);
                TypeRef returnType = ResolveOperatorReturnType(objectType, *method);
                if (parameterTypes.size() == 1 && !parameterTypes[0].IsUnknown() &&
                    !CanAssignExprTo(*index->index, indexType, parameterTypes[0])) {
                    EmitError(index->index->location, std::format("cannot pass '{}' to parameter of type '{}'",
                                                                  indexType.ToString(), parameterTypes[0].ToString()));
                }
                indexOperators.insert_or_assign(index,
                                                ResolvedIndexOperator{method, objectType, indexType, returnType});
                return returnType;
            }
        }

        if (readsPlace) {
            CheckBorrowedPlaceRead(*index, index->location);
        }
        // A named type that reaches an index error could have declared the operator, so its diagnostics name that
        // route instead of only listing the forms the language indexes on its own.
        const bool isNamedType = !builtinElementType && !NamedBaseTypeName(objectType).empty();
        if (indexType.IsRange()) {
            std::optional<TypeRef> elementType;
            if (objectValueType.kind == TypeRef::Kind::Array && !objectValueType.inner.empty()) {
                elementType = objectValueType.inner[0];
            }
            else {
                elementType = SliceElementType(objectValueType);
            }
            if (elementType) {
                return TypeRef::MakeSlice(*elementType);
            }
            if (objectValueType.IsString()) {
                EmitError(index->location,
                          std::format("cannot take a range of string type '{}'", objectType.ToString()),
                          {"a sub-range could split one character's code units and leave text that is not valid"},
                          "index a single code unit with '[i]', or build a slice from '.data' and '.length'");
                return TypeRef::MakeUnknown();
            }
            std::optional<std::string> sliceHelp;
            if (isNamedType) {
                sliceHelp = std::format("declare 'func []' taking a range on '{}'", objectType.ToString());
            }
            EmitError(index->location, std::format("cannot slice value of type '{}'", objectType.ToString()), {},
                      std::move(sliceHelp));
            return TypeRef::MakeUnknown();
        }
        if (!indexType.IsUnknown() && !indexType.IsInteger() && !isNamedType) {
            // A named type with no `[]` is reported as unindexable below; only the built-in forms, which do fix the
            // index type, report a wrong index type here.
            EmitError(index->index->location,
                      std::format("index for type '{}' must be an integer or range, but has type '{}'",
                                  objectType.ToString(), indexType.ToString()));
            return TypeRef::MakeUnknown();
        }
        if (builtinElementType) {
            return *builtinElementType;
        }
        if (!objectType.IsUnknown()) {
            EmitError(index->object->location, std::format("type '{}' cannot be indexed", objectType.ToString()), {},
                      isNamedType ? std::format("declare 'func []' on '{}'", objectType.ToString())
                                  : std::string("only arrays, slices, and pointers support indexing"));
        }
        return TypeRef::MakeUnknown();
    }

    if (const auto *field = dynamic_cast<const FieldExpr *>(&expression)) {
        const bool wasProjectionRoot = checkingBorrowProjectionRoot;
        checkingBorrowProjectionRoot = true;
        const TypeRef objectType = CheckExpr(*field->object);
        checkingBorrowProjectionRoot = wasProjectionRoot;
        if (!wasProjectionRoot && !checkingPlainAssignmentTarget) {
            CheckBorrowedPlaceRead(*field, field->location);
        }
        const TypeRef &objectValueType = ReferencedValue(objectType);
        if (objectValueType.kind == TypeRef::Kind::Array && objectValueType.arrayLength && field->field == "length") {
            return TypeRef::MakeUInt();
        }
        if (auto elementType = SliceElementType(objectValueType)) {
            if (!VisibleIntrinsicType(objectValueType, field->location)) {
                return TypeRef::MakeUnknown();
            }
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
        // A visible declaration grants access to the compiler-owned text representation.
        if (objectValueType.IsString()) {
            if (!VisibleIntrinsicType(objectValueType, field->location)) {
                return TypeRef::MakeUnknown();
            }
            if (field->field == "data") {
                return TypeRef::MakePointer(TypeRef::MakePrimitive(StringCodeUnitKind(objectValueType.kind)));
            }
            if (field->field == "length") {
                return TypeRef::MakeUInt();
            }
            EmitError(field->location,
                      std::format("string type '{}' has no member '{}'", objectType.ToString(), field->field),
                      {"available string members are 'data' and 'length'"});
            return TypeRef::MakeUnknown();
        }
        if (objectValueType.IsRange()) {
            if (!VisibleIntrinsicType(objectValueType, field->location)) {
                return TypeRef::MakeUnknown();
            }
            const TypeRef elementType = objectValueType.inner.empty() ? TypeRef::MakeInt64() : objectValueType.inner[0];
            if (field->field == "start" && objectValueType.RangeHasStart()) {
                return elementType;
            }
            if (field->field == "end" && objectValueType.RangeHasEnd()) {
                return elementType;
            }
            EmitError(field->location,
                      std::format("range type '{}' has no member '{}'", objectType.ToString(), field->field),
                      {"range members are 'start' and 'end' when that bound is present"});
            return TypeRef::MakeUnknown();
        }
        if (objectValueType.kind == TypeRef::Kind::Tuple) {
            try {
                std::size_t consumed = 0;
                const std::size_t index = std::stoul(field->field, &consumed);
                if (consumed == field->field.size()) {
                    if (index < objectValueType.inner.size()) {
                        return objectValueType.inner[index];
                    }
                    EmitError(field->location,
                              std::format("tuple index {} is out of range for a tuple with {} element{}", index,
                                          objectValueType.inner.size(), objectValueType.inner.size() == 1 ? "" : "s"));
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
            const StructDecl &declaration = *structDecls.at(structName);
            const auto member = std::ranges::find_if(
                declaration.fields, [&](const StructDecl::Field &candidate) { return candidate.name == field->field; });
            if (member != declaration.fields.end() && !IsMemberAccessible(declaration, member->isPublic)) {
                EmitPrivateMemberError(field->location, declaration, "struct field", field->field);
                return TypeRef::MakeUnknown();
            }
            if (TypeRef fieldType = StructFieldType(objectType, field->field); !fieldType.IsUnknown()) {
                return fieldType;
            }
            EmitError(
                field->location, std::format("struct '{}' has no field '{}'", objectType.ToString(), field->field),
                {std::format("available fields are {}",
                             AvailableNamesIf(
                                 declaration.fields, [](const StructDecl::Field &candidate) { return candidate.name; },
                                 [this, &declaration](const StructDecl::Field &candidate) {
                                     return IsMemberAccessible(declaration, candidate.isPublic);
                                 }))});
            return TypeRef::MakeUnknown();
        }
        if (!structName.empty() && unionDecls.contains(structName)) {
            const UnionDecl &declaration = *unionDecls.at(structName);
            const auto member = std::ranges::find_if(
                declaration.fields, [&](const UnionDecl::Field &candidate) { return candidate.name == field->field; });
            if (member != declaration.fields.end() && !IsMemberAccessible(declaration, member->isPublic)) {
                EmitPrivateMemberError(field->location, declaration, "union field", field->field);
                return TypeRef::MakeUnknown();
            }
            if (TypeRef fieldType = StructFieldType(objectType, field->field); !fieldType.IsUnknown()) {
                return fieldType;
            }
            EmitError(
                field->location, std::format("union '{}' has no field '{}'", objectType.ToString(), field->field),
                {std::format("available fields are {}",
                             AvailableNamesIf(
                                 declaration.fields, [](const UnionDecl::Field &candidate) { return candidate.name; },
                                 [this, &declaration](const UnionDecl::Field &candidate) {
                                     return IsMemberAccessible(declaration, candidate.isPublic);
                                 }))});
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
        return type.IsRange() || type.isIntrinsicSlice ? type : TypeRef::MakeNamed(typeName);
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
        TypeRef arrayType = TypeRef::MakeArray(elementType, array->elements.size());
        ValidateStoredType(arrayType, array->location, "array value");
        return arrayType;
    }

    if (const auto *repeat = dynamic_cast<const ArrayRepeatExpr *>(&expression)) {
        const TypeRef elementType = CheckExpr(*repeat->value);
        const std::optional<std::uint64_t> count = EvalArrayLength(*repeat->count);
        if (!count) {
            EmitError(repeat->count->location, "array repeat count must be a non-negative compile-time integer");
        }
        if (!elementType.IsUnknown()) {
            ConsumeValue(*repeat->value, elementType, ValueConsumptionKind::ArrayRepeat, repeat->value->location);
        }
        TypeRef arrayType = TypeRef::MakeArray(elementType, count.value_or(0));
        ValidateStoredType(arrayType, repeat->location, "array value");
        return arrayType;
    }

    if (const auto *tuple = dynamic_cast<const TupleExpr *>(&expression)) {
        std::vector<TypeRef> elementTypes;
        for (const auto &element : tuple->elements) {
            const TypeRef type = CheckExpr(*element);
            ConsumeValue(*element, type, ValueConsumptionKind::Aggregate, element->location);
            elementTypes.push_back(type);
        }
        TypeRef tupleType = TypeRef::MakeTuple(std::move(elementTypes));
        ValidateStoredType(tupleType, tuple->location, "tuple value");
        return tupleType;
    }

    return std::nullopt;
}
} // namespace Rux::SemanticDetail
