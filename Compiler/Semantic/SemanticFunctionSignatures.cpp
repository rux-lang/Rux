// Function signature construction, duplicate-overload validation, and the
// module signature pass shared by ordinary and dependency packages.

#include "Semantic/Detail/SemanticAnalyzerContext.h"

#include <format>

namespace Rux::SemanticDetail {
TypeRef SemanticAnalyzerContext::MakeFuncType(const std::vector<Param> &parameters,
                                              const std::optional<TypeExprPtr> &returnType,
                                              const std::vector<std::string> &typeParameters, const bool cVariadic) {
    const auto savedTypeParameters = currentTypeParams;
    currentTypeParams = typeParameters;

    std::vector<TypeRef> parameterTypes;
    bool variadic = cVariadic;
    for (const Param &parameter : parameters) {
        if (!parameter.isVariadic) {
            parameterTypes.push_back(ResolveType(*parameter.type));
        }
        else {
            variadic = true;
        }
    }
    TypeRef resolvedReturn = returnType ? ResolveType(**returnType) : TypeRef::MakeOpaque();

    currentTypeParams = savedTypeParameters;
    TypeRef functionType = TypeRef::MakeFunc(std::move(parameterTypes), std::move(resolvedReturn));
    functionType.isVariadic = variadic;
    return functionType;
}

TypeRef SemanticAnalyzerContext::MakeFuncTypeWithSubstitution(
    const std::vector<Param> &parameters, const std::optional<TypeExprPtr> &returnType,
    const std::unordered_map<std::string, TypeRef> &substitutions, const std::vector<std::string> &typeParameters,
    const bool cVariadic) {
    const auto savedTypeParameters = currentTypeParams;
    currentTypeParams = typeParameters;

    std::vector<TypeRef> parameterTypes;
    bool variadic = cVariadic;
    for (const Param &parameter : parameters) {
        if (!parameter.isVariadic) {
            parameterTypes.push_back(ResolveTypeWithSubstitution(*parameter.type, substitutions));
        }
        else {
            variadic = true;
        }
    }
    TypeRef resolvedReturn =
        returnType ? ResolveTypeWithSubstitution(**returnType, substitutions) : TypeRef::MakeOpaque();

    currentTypeParams = savedTypeParameters;
    TypeRef functionType = TypeRef::MakeFunc(std::move(parameterTypes), std::move(resolvedReturn));
    functionType.isVariadic = variadic;
    return functionType;
}

void SemanticAnalyzerContext::ResolveModuleSignatures(const Module &module) {
    currentFile = module.name;
    for (const auto &declaration : module.items) {
        ResolveDeclSignature(*declaration);
    }
}

void SemanticAnalyzerContext::ResolveDeclSignature(const Decl &declaration) {
    if (const auto *function = dynamic_cast<const FuncDecl *>(&declaration)) {
        if (Symbol *symbol = globalScope.Lookup(function->name)) {
            symbol->type =
                MakeFuncType(function->params, function->returnType, TypeParameterNames(function->typeParams));
        }
    }
    else if (const auto *enumeration = dynamic_cast<const EnumDecl *>(&declaration)) {
        if (Symbol *symbol = globalScope.Lookup(enumeration->name)) {
            symbol->type = EnumType(*enumeration);
        }
    }
    else if (const auto *externFunction = dynamic_cast<const ExternFuncDecl *>(&declaration)) {
        if (Symbol *symbol = globalScope.Lookup(externFunction->name)) {
            symbol->type =
                MakeFuncType(externFunction->params, externFunction->returnType, {}, externFunction->isVariadic);
        }
    }
    else if (const auto *block = dynamic_cast<const ExternBlockDecl *>(&declaration)) {
        for (const auto &item : block->items) {
            ResolveDeclSignature(*item);
        }
    }
    else if (const auto *nested = dynamic_cast<const ModuleDecl *>(&declaration)) {
        Scope &moduleScope = programIndex.ModuleScopeFor(nested->name, globalScope);
        for (const auto &item : nested->items) {
            ResolveDeclSignatureInScope(*item, moduleScope);
        }
    }
}

void SemanticAnalyzerContext::ResolveModuleSignaturesInScope(const Module &module, Scope &scope) {
    Scope *savedScope = currentScope;
    currentScope = &scope;
    currentFile = module.name;
    for (const auto &declaration : module.items) {
        ResolveDeclSignatureInScope(*declaration, scope);
    }
    currentScope = savedScope;
}

void SemanticAnalyzerContext::ResolveDeclSignatureInScope(const Decl &declaration, Scope &scope) {
    if (const auto *function = dynamic_cast<const FuncDecl *>(&declaration)) {
        if (Symbol *symbol = scope.Lookup(function->name)) {
            symbol->type =
                MakeFuncType(function->params, function->returnType, TypeParameterNames(function->typeParams));
        }
    }
    else if (const auto *enumeration = dynamic_cast<const EnumDecl *>(&declaration)) {
        if (Symbol *symbol = scope.Lookup(enumeration->name)) {
            symbol->type = EnumType(*enumeration);
        }
    }
    else if (const auto *externFunction = dynamic_cast<const ExternFuncDecl *>(&declaration)) {
        if (Symbol *symbol = scope.Lookup(externFunction->name)) {
            symbol->type =
                MakeFuncType(externFunction->params, externFunction->returnType, {}, externFunction->isVariadic);
        }
    }
    else if (const auto *block = dynamic_cast<const ExternBlockDecl *>(&declaration)) {
        for (const auto &item : block->items) {
            ResolveDeclSignatureInScope(*item, scope);
        }
    }
    else if (const auto *nested = dynamic_cast<const ModuleDecl *>(&declaration)) {
        Scope &moduleScope = programIndex.ModuleScopeFor(nested->name, scope);
        for (const auto &item : nested->items) {
            ResolveDeclSignatureInScope(*item, moduleScope);
        }
    }
}

std::optional<SemanticAnalyzerContext::FunctionSignature>
SemanticAnalyzerContext::ResolveFunctionSignature(const FuncDecl &declaration, const bool isMethod) {
    const auto savedTypeParameters = currentTypeParams;
    if (!isMethod) {
        currentTypeParams.clear();
    }
    AppendTypeParameterNames(currentTypeParams, declaration.typeParams);

    std::unordered_map<std::string, TypeRef> substitutions;
    for (std::size_t index = 0; index < declaration.typeParams.size(); ++index) {
        substitutions.emplace(declaration.typeParams[index].name, TypeRef::MakeTypeParam(std::format("${}", index)));
    }

    FunctionSignature signature;
    signature.typeParamCount = declaration.typeParams.size();
    for (const Param &parameter : declaration.params) {
        if (isMethod && parameter.name == "self") {
            continue;
        }
        TypeRef type = ResolveTypeWithSubstitution(*parameter.type, substitutions);
        if (type.IsUnknown()) {
            currentTypeParams = savedTypeParameters;
            return std::nullopt;
        }
        signature.paramTypes.push_back(std::move(type));
        signature.variadicParams.push_back(parameter.isVariadic);
    }

    currentTypeParams = savedTypeParameters;
    return signature;
}

bool SemanticAnalyzerContext::SameFunctionSignature(const FunctionSignature &left, const FunctionSignature &right) {
    return left.typeParamCount == right.typeParamCount && left.paramTypes == right.paramTypes &&
           left.variadicParams == right.variadicParams;
}

void SemanticAnalyzerContext::ValidateFunctionSignature(const FuncDecl &declaration,
                                                        const std::vector<const FuncDecl *> &overloads,
                                                        const bool isMethod) {
    const auto signature = ResolveFunctionSignature(declaration, isMethod);
    if (!signature) {
        return;
    }

    for (const FuncDecl *previous : overloads) {
        if (previous == &declaration) {
            break;
        }
        const auto previousSignature = ResolveFunctionSignature(*previous, isMethod);
        if (previousSignature && SameFunctionSignature(*signature, *previousSignature)) {
            const auto source = functionDeclFiles.find(previous);
            const std::string &previousFile = source == functionDeclFiles.end() ? currentFile : source->second;
            EmitError(
                declaration.location,
                std::format("function '{}' has the same parameter signature as an earlier overload", declaration.name),
                {std::format("the earlier overload was declared at '{}':{}:{}", previousFile, previous->location.line,
                             previous->location.column)});
            return;
        }
    }
}

void SemanticAnalyzerContext::CheckModule(const Module &module) {
    currentFile = module.name;
    for (const auto &declaration : module.items) {
        CheckDecl(*declaration);
    }
}

void SemanticAnalyzerContext::CheckModuleInScope(const Module &module, Scope &scope) {
    Scope *savedScope = currentScope;
    currentScope = &scope;
    CheckModule(module);
    currentScope = savedScope;
}
} // namespace Rux::SemanticDetail
