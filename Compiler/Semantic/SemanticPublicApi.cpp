// Effective public-API closure validation. This is separate from declaration
// checking because it owns one cross-cutting policy over every declaration kind.

#include "Semantic/Detail/SemanticAnalyzerContext.h"

#include <format>

namespace Rux::SemanticDetail {
void SemanticAnalyzerContext::ValidatePublicType(const TypeExpr &type, const std::string_view subject,
                                                 const std::unordered_set<std::string> &typeParameters) {
    if (const auto *named = dynamic_cast<const NamedTypeExpr *>(&type)) {
        if (!typeParameters.contains(named->name)) {
            if (const Symbol *symbol = currentScope->Lookup(named->name);
                symbol && symbol->ownerPackage != "<builtin>" && !symbol->isEffectivelyPublic &&
                reportedPrivateApiTypes.insert(&type).second) {
                EmitError(type.location,
                          std::format("{} exposes private {} '{}'", subject, SymbolKindName(symbol->kind), named->name),
                          {DeclarationNote(*symbol)},
                          std::format("make '{}' public or remove it from the public signature", named->name));
            }
        }
        for (const auto &argument : named->typeArgs) {
            ValidatePublicType(*argument, subject, typeParameters);
        }
        return;
    }
    if (const auto *pointer = dynamic_cast<const PointerTypeExpr *>(&type)) {
        ValidatePublicType(*pointer->pointee, subject, typeParameters);
    }
    else if (const auto *reference = dynamic_cast<const ReferenceTypeExpr *>(&type)) {
        ValidatePublicType(*reference->pointee, subject, typeParameters);
    }
    else if (const auto *array = dynamic_cast<const ArrayTypeExpr *>(&type)) {
        ValidatePublicType(*array->element, subject, typeParameters);
    }
    else if (const auto *tuple = dynamic_cast<const TupleTypeExpr *>(&type)) {
        for (const auto &element : tuple->elements) {
            ValidatePublicType(*element, subject, typeParameters);
        }
    }
    else if (const auto *function = dynamic_cast<const FunctionTypeExpr *>(&type)) {
        for (const auto &parameter : function->params) {
            ValidatePublicType(*parameter, subject, typeParameters);
        }
        if (function->returnType) {
            ValidatePublicType(**function->returnType, subject, typeParameters);
        }
    }
}

void SemanticAnalyzerContext::ValidatePublicResolvedType(const TypeRef &type, const Decl &declaration,
                                                         const std::string_view subject) {
    if (type.kind == TypeRef::Kind::Named) {
        const std::string name = BaseTypeName(type.name);
        if (const Symbol *symbol = currentScope->Lookup(name);
            symbol && symbol->ownerPackage != "<builtin>" && !symbol->isEffectivelyPublic &&
            reportedPrivateApiDeclarations.insert(&declaration).second) {
            EmitError(declaration.location,
                      std::format("{} exposes private {} '{}'", subject, SymbolKindName(symbol->kind), name),
                      {DeclarationNote(*symbol)},
                      std::format("make '{}' public or remove it from the public signature", name));
        }
        for (const TypeRef &argument : ParseTypeArgsFromTypeName(type.name)) {
            ValidatePublicResolvedType(argument, declaration, subject);
        }
    }
    for (const TypeRef &inner : type.inner) {
        ValidatePublicResolvedType(inner, declaration, subject);
    }
}

void SemanticAnalyzerContext::ValidatePublicTypeParameters(const std::vector<TypeParameter> &parameters,
                                                           const std::string_view subject,
                                                           const std::unordered_set<std::string> &typeParameterNames) {
    for (const TypeParameter &parameter : parameters) {
        for (const auto &bound : parameter.bounds) {
            ValidatePublicType(*bound, subject, typeParameterNames);
        }
    }
}

void SemanticAnalyzerContext::ValidatePublicFunction(const FuncDecl &function, const std::string_view subject,
                                                     const TypeExpr *extendedType) {
    const auto info = declarationInfos.find(&function);
    if (info != declarationInfos.end() && !info->second.isEffectivelyPublic) {
        return;
    }
    std::unordered_set<std::string> typeParameters(currentTypeParams.begin(), currentTypeParams.end());
    for (const TypeParameter &parameter : function.typeParams) {
        typeParameters.insert(parameter.name);
    }
    ValidatePublicTypeParameters(function.typeParams, subject, typeParameters);
    if (extendedType) {
        ValidatePublicType(*extendedType, subject, typeParameters);
    }
    for (const Param &parameter : function.params) {
        if (!parameter.IsReceiver()) {
            ValidatePublicType(*parameter.type, subject, typeParameters);
        }
    }
    if (function.returnType) {
        ValidatePublicType(**function.returnType, subject, typeParameters);
    }
}

void SemanticAnalyzerContext::ValidatePublicDeclaration(const Decl &declaration) {
    const auto info = declarationInfos.find(&declaration);
    if (info == declarationInfos.end() || !info->second.isEffectivelyPublic) {
        return;
    }
    if (const auto *function = dynamic_cast<const FuncDecl *>(&declaration)) {
        ValidatePublicFunction(*function, std::format("public function '{}'", function->name));
    }
    else if (const auto *structure = dynamic_cast<const StructDecl *>(&declaration)) {
        std::unordered_set<std::string> parameters;
        for (const TypeParameter &parameter : structure->typeParams) {
            parameters.insert(parameter.name);
        }
        ValidatePublicTypeParameters(structure->typeParams, std::format("public struct '{}'", structure->name),
                                     parameters);
        for (const StructDecl::Field &field : structure->fields) {
            if (field.isPublic) {
                ValidatePublicType(*field.type, std::format("public field '{}.{}'", structure->name, field.name),
                                   parameters);
            }
        }
    }
    else if (const auto *enumeration = dynamic_cast<const EnumDecl *>(&declaration)) {
        std::unordered_set<std::string> parameters;
        for (const TypeParameter &parameter : enumeration->typeParams) {
            parameters.insert(parameter.name);
        }
        ValidatePublicTypeParameters(enumeration->typeParams, std::format("public enum '{}'", enumeration->name),
                                     parameters);
        for (const EnumDecl::Variant &variant : enumeration->variants) {
            for (const auto &field : variant.fields) {
                ValidatePublicType(*field, std::format("public enum variant '{}::{}'", enumeration->name, variant.name),
                                   parameters);
            }
            for (const EnumDecl::Variant::NamedField &field : variant.namedFields) {
                ValidatePublicType(
                    *field.type,
                    std::format("public enum field '{}::{}.{}'", enumeration->name, variant.name, field.name),
                    parameters);
            }
        }
    }
    else if (const auto *unionType = dynamic_cast<const UnionDecl *>(&declaration)) {
        for (const UnionDecl::Field &field : unionType->fields) {
            if (field.isPublic) {
                ValidatePublicType(*field.type, std::format("public union field '{}.{}'", unionType->name, field.name));
            }
        }
    }
    else if (const auto *interface = dynamic_cast<const InterfaceDecl *>(&declaration)) {
        for (const auto &method : interface->methods) {
            ValidatePublicFunction(*method,
                                   std::format("public interface requirement '{}.{}'", interface->name, method->name));
        }
    }
    else if (const auto *constant = dynamic_cast<const ConstDecl *>(&declaration)) {
        if (constant->type) {
            ValidatePublicType(**constant->type, std::format("public constant '{}'", constant->name));
        }
        else if (const Symbol *symbol = currentScope->LookupLocal(constant->name)) {
            ValidatePublicResolvedType(symbol->type, declaration, std::format("public constant '{}'", constant->name));
        }
    }
    else if (const auto *alias = dynamic_cast<const TypeAliasDecl *>(&declaration)) {
        ValidatePublicType(*alias->type, std::format("public type alias '{}'", alias->name));
    }
    else if (const auto *externFunction = dynamic_cast<const ExternFuncDecl *>(&declaration)) {
        for (const Param &parameter : externFunction->params) {
            ValidatePublicType(*parameter.type, std::format("public extern function '{}'", externFunction->name));
        }
        if (externFunction->returnType) {
            ValidatePublicType(**externFunction->returnType,
                               std::format("public extern function '{}'", externFunction->name));
        }
    }
    else if (const auto *variable = dynamic_cast<const ExternVarDecl *>(&declaration)) {
        ValidatePublicType(*variable->type, std::format("public extern variable '{}'", variable->name));
    }
}
} // namespace Rux::SemanticDetail
