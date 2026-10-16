// Function signature construction, duplicate-overload validation, and the
// module signature pass shared by ordinary and dependency packages.

#include "Semantic/Analysis/AnalysisContext.h"

#include <algorithm>
#include <format>

namespace Rux::SemanticDetail {
bool HasCAbiMapping(const TypeRef &type) noexcept {
    return !type.IsFloat() || type->kind == TypeRef::Kind::Float32 || type->kind == TypeRef::Kind::Float64;
}

std::optional<AnalysisContext::CAbiViolation>
AnalysisContext::FindCAbiViolation(const TypeRef &type, std::unordered_set<std::string> &visiting) {
    if (!HasCAbiMapping(type)) {
        return CAbiViolation{"", type};
    }
    // Prefix a violation found inside a member with the member's name.
    const auto inside = [](std::optional<CAbiViolation> found, const std::string_view member) {
        if (found) {
            found->path = found->path.empty() ? std::string(member) : std::format("{}.{}", member, found->path);
        }
        return found;
    };
    switch (type->kind) {
    case TypeRef::Kind::Array:
        // A fixed array is its elements in place; the field holding it is the path to them.
        if (type->arrayLength && !type->inner.empty()) {
            return FindCAbiViolation(type->inner.front(), visiting);
        }
        return std::nullopt;
    case TypeRef::Kind::Tuple:
        for (std::size_t index = 0; index < type->inner.size(); ++index) {
            if (auto found = inside(FindCAbiViolation(type->inner[index], visiting), std::to_string(index))) {
                return found;
            }
        }
        return std::nullopt;
    case TypeRef::Kind::Named:
        break;
    default:
        // A pointer, a reference and a slice cross as addresses; nothing they point at is passed.
        return std::nullopt;
    }

    const std::string baseName = BaseTypeName(type->name);
    // A struct cannot hold itself by value; the guard only keeps a malformed declaration from recursing forever.
    if (!visiting.insert(type->name).second) {
        return std::nullopt;
    }
    std::optional<CAbiViolation> found;
    if (const auto structure = structDecls.find(baseName); structure != structDecls.end()) {
        std::unordered_map<std::string, TypeRef> substitutions;
        const std::vector<TypeRef> typeArgs = ParseTypeArgsFromTypeName(type->name);
        const auto &typeParams = structure->second->typeParams;
        for (std::size_t index = 0; index < std::min(typeParams.size(), typeArgs.size()); ++index) {
            substitutions[typeParams[index].name] = typeArgs[index];
        }
        for (const auto &field : structure->second->fields) {
            found = inside(FindCAbiViolation(ResolveTypeWithSubstitution(*field.type, substitutions), visiting),
                           field.name);
            if (found) {
                break;
            }
        }
    }
    else if (const auto unionType = unionDecls.find(baseName); unionType != unionDecls.end()) {
        for (const auto &field : unionType->second->fields) {
            found = inside(FindCAbiViolation(ResolveTypeWithSubstitution(*field.type, {}), visiting), field.name);
            if (found) {
                break;
            }
        }
    }
    visiting.erase(type->name);
    return found;
}

void AnalysisContext::ValidateCAbiType(const TypeRef &type, const SourceLocation location,
                                       const std::string_view subject) {
    std::unordered_set<std::string> visiting;
    const auto violation = FindCAbiViolation(type, visiting);
    if (!violation) {
        return;
    }
    const std::vector<std::string> notes = {
        std::format("'{}' is passed by the compiler's own convention, which matches no C type such as "
                    "'_Float16', 'long double' or '__float128'",
                    violation->type.ToString())};
    if (violation->path.empty()) {
        EmitError(location, std::format("{} has type '{}', which has no C ABI mapping", subject, type.ToString()),
                  notes, "convert the value to 'float32' or 'float64' at the boundary, or pass a pointer to it");
        return;
    }
    // A by-value aggregate is laid out in C's registers and memory field by field, so one unmappable field anywhere
    // inside it makes the whole value one the other side reads differently.
    EmitError(location,
              std::format("{} has type '{}', whose field '{}' holds '{}', which has no C ABI mapping", subject,
                          type.ToString(), violation->path, violation->type.ToString()),
              notes, "store the field as 'float32' or 'float64', or pass a pointer to the value");
}

TypeRef AnalysisContext::MakeFuncType(const std::vector<Param> &parameters,
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
    TypeRef functionType = TypeRef::MakeFunc(std::move(parameterTypes), resolvedReturn);
    functionType.isVariadic = variadic;
    return functionType;
}

TypeRef AnalysisContext::MakeFuncTypeWithSubstitution(const std::vector<Param> &parameters,
                                                      const std::optional<TypeExprPtr> &returnType,
                                                      const std::unordered_map<std::string, TypeRef> &substitutions,
                                                      const std::vector<std::string> &typeParameters,
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
    TypeRef functionType = TypeRef::MakeFunc(std::move(parameterTypes), resolvedReturn);
    functionType.isVariadic = variadic;
    return functionType;
}

void AnalysisContext::ResolveModuleSignatures(const Module &module) {
    currentFile = module.name;
    Scope *const savedScope = currentScope;
    currentScope = &programIndex.FileScopeFor(module.name, *currentScope);
    for (const auto &declaration : module.items) {
        ResolveDeclSignature(*declaration);
    }
    currentScope = savedScope;
}

void AnalysisContext::ResolveDeclSignature(const Decl &declaration) {
    if (const auto *function = AstCast<const FuncDecl>(&declaration)) {
        if (Symbol *symbol = globalScope.Lookup(function->name)) {
            symbol->type =
                MakeFuncType(function->params, function->returnType, TypeParameterNames(function->typeParams));
        }
    }
    else if (const auto *extension = AstCast<const ImplDecl>(&declaration)) {
        // CheckImplDecl reports the block; resolving its signatures would only repeat that its element is undefined.
        if (!UndefinedSliceElement(*extension).empty()) {
            return;
        }
        const auto receiverParameters = ImplTypeParams(*extension);
        const auto savedParameters = currentTypeParams;
        currentTypeParams = receiverParameters;
        const Symbol *symbol = currentScope->Lookup(BaseTypeName(extension->typeName));
        // A native target is rejected where the extension is checked, so it binds no methods here.
        const bool canResolve = extension->extendedType && !IsNativeTypeExpr(*extension->extendedType) &&
                                ((symbol && symbol->kind == Symbol::Kind::Type) ||
                                 !AstCast<const NamedTypeExpr>(extension->extendedType.get()));
        const TypeRef receiver =
            extension->extendedType && canResolve ? ResolveType(*extension->extendedType) : TypeRef::MakeUnknown();
        currentTypeParams = savedParameters;
        if (!receiver.IsUnknown()) {
            programIndex.BindImplementationMethods(*extension, NamedBaseTypeName(receiver),
                                                   extension->interfaceName ? NominalTypeName(*extension->interfaceName)
                                                                            : std::string{});
            // The methods of a read-only view are a writable view's too; those of `extend var T[..]` stay its own.
            if (receiver.IsSlice() && !receiver.IsWritableSlice()) {
                programIndex.ShareImplementationMethods(*extension, receiver.WithWritableInner(true).ToString());
            }
        }
        for (const auto &method : extension->methods) {
            auto parameters = receiverParameters;
            AppendTypeParameterNames(parameters, method->typeParams);
            (void)MakeFuncType(method->params, method->returnType, parameters);
        }
    }
    else if (const auto *enumeration = AstCast<const EnumDecl>(&declaration)) {
        if (Symbol *symbol = globalScope.Lookup(enumeration->name)) {
            symbol->type = EnumType(*enumeration);
        }
    }
    else if (const auto *externFunction = AstCast<const ExternFuncDecl>(&declaration)) {
        if (Symbol *symbol = globalScope.Lookup(externFunction->name)) {
            symbol->type =
                MakeFuncType(externFunction->params, externFunction->returnType, {}, externFunction->isVariadic);
        }
    }
    else if (const auto *block = AstCast<const ExternBlockDecl>(&declaration)) {
        for (const auto &item : block->items) {
            ResolveDeclSignature(*item);
        }
    }
    else if (const auto *nested = AstCast<const ModuleDecl>(&declaration)) {
        Scope &moduleScope = programIndex.ModuleScopeFor(nested->name, globalScope);
        Scope *savedScope = currentScope;
        currentScope = &ModuleScopeFor(nested->name, *currentScope);
        for (const auto &item : nested->items) {
            ResolveDeclSignatureInScope(*item, moduleScope);
        }
        currentScope = savedScope;
    }
}

void AnalysisContext::ResolveModuleSignaturesInScope(const Module &module, Scope &scope) {
    Scope *savedScope = currentScope;
    currentScope = &programIndex.FileScopeFor(module.name, scope);
    currentFile = module.name;
    for (const auto &declaration : module.items) {
        ResolveDeclSignatureInScope(*declaration, scope);
    }
    currentScope = savedScope;
}

void AnalysisContext::ResolveDeclSignatureInScope(const Decl &declaration, Scope &scope) {
    if (const auto *function = AstCast<const FuncDecl>(&declaration)) {
        if (Symbol *symbol = scope.Lookup(function->name)) {
            symbol->type =
                MakeFuncType(function->params, function->returnType, TypeParameterNames(function->typeParams));
        }
    }
    else if (const auto *extension = AstCast<const ImplDecl>(&declaration)) {
        // CheckImplDecl reports the block; resolving its signatures would only repeat that its element is undefined.
        if (!UndefinedSliceElement(*extension).empty()) {
            return;
        }
        const auto receiverParameters = ImplTypeParams(*extension);
        const auto savedParameters = currentTypeParams;
        currentTypeParams = receiverParameters;
        const Symbol *symbol = currentScope->Lookup(BaseTypeName(extension->typeName));
        // A native target is rejected where the extension is checked, so it binds no methods here.
        const bool canResolve = extension->extendedType && !IsNativeTypeExpr(*extension->extendedType) &&
                                ((symbol && symbol->kind == Symbol::Kind::Type) ||
                                 !AstCast<const NamedTypeExpr>(extension->extendedType.get()));
        const TypeRef receiver =
            extension->extendedType && canResolve ? ResolveType(*extension->extendedType) : TypeRef::MakeUnknown();
        currentTypeParams = savedParameters;
        if (!receiver.IsUnknown()) {
            programIndex.BindImplementationMethods(*extension, NamedBaseTypeName(receiver),
                                                   extension->interfaceName ? NominalTypeName(*extension->interfaceName)
                                                                            : std::string{});
            // The methods of a read-only view are a writable view's too; those of `extend var T[..]` stay its own.
            if (receiver.IsSlice() && !receiver.IsWritableSlice()) {
                programIndex.ShareImplementationMethods(*extension, receiver.WithWritableInner(true).ToString());
            }
        }
        for (const auto &method : extension->methods) {
            auto parameters = receiverParameters;
            AppendTypeParameterNames(parameters, method->typeParams);
            (void)MakeFuncType(method->params, method->returnType, parameters);
        }
    }
    else if (const auto *enumeration = AstCast<const EnumDecl>(&declaration)) {
        if (Symbol *symbol = scope.Lookup(enumeration->name)) {
            symbol->type = EnumType(*enumeration);
        }
    }
    else if (const auto *externFunction = AstCast<const ExternFuncDecl>(&declaration)) {
        if (Symbol *symbol = scope.Lookup(externFunction->name)) {
            symbol->type =
                MakeFuncType(externFunction->params, externFunction->returnType, {}, externFunction->isVariadic);
        }
    }
    else if (const auto *block = AstCast<const ExternBlockDecl>(&declaration)) {
        for (const auto &item : block->items) {
            ResolveDeclSignatureInScope(*item, scope);
        }
    }
    else if (const auto *nested = AstCast<const ModuleDecl>(&declaration)) {
        Scope &moduleScope = programIndex.ModuleScopeFor(nested->name, scope);
        Scope *savedScope = currentScope;
        currentScope = &ModuleScopeFor(nested->name, *currentScope);
        for (const auto &item : nested->items) {
            ResolveDeclSignatureInScope(*item, moduleScope);
        }
        currentScope = savedScope;
    }
}

std::optional<AnalysisContext::FunctionSignature> AnalysisContext::ResolveFunctionSignature(const FuncDecl &declaration,
                                                                                            const bool isMethod) {
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
        signature.paramTypes.push_back(type);
        signature.variadicParams.push_back(parameter.isVariadic);
    }

    currentTypeParams = savedTypeParameters;
    return signature;
}

bool AnalysisContext::SameFunctionSignature(const FunctionSignature &left, const FunctionSignature &right) {
    return left.typeParamCount == right.typeParamCount && left.paramTypes == right.paramTypes &&
           left.variadicParams == right.variadicParams;
}

void AnalysisContext::ValidateFunctionSignature(const FuncDecl &declaration,
                                                const std::vector<const FuncDecl *> &overloads, const bool isMethod) {
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

void AnalysisContext::CheckModule(const Module &module) {
    currentFile = module.name;
    Scope *const savedScope = currentScope;
    currentScope = &programIndex.FileScopeFor(module.name, *currentScope);
    checkingDeclarationsOnly = declarationOnlyModules != nullptr && declarationOnlyModules->contains(&module);
    for (const auto &declaration : module.items) {
        CheckDecl(*declaration);
    }
    checkingDeclarationsOnly = false;
    currentScope = savedScope;
}

void AnalysisContext::PrecheckModule(const Module &module) {
    currentFile = module.name;
    // A reused module's constants are checked when something checked here reads them: its own code is reused, so
    // nothing else needs them, and a generated table can hold many thousands of elements.
    checkingDeclarationsOnly = declarationOnlyModules != nullptr && declarationOnlyModules->contains(&module);
    Scope *const savedScope = currentScope;
    currentScope = &programIndex.FileScopeFor(module.name, *currentScope);
    for (const auto &declaration : module.items) {
        PrecheckDecl(*declaration);
    }
    checkingDeclarationsOnly = false;
    currentScope = savedScope;
}

void AnalysisContext::PrecheckModuleInScope(const Module &module, Scope &scope) {
    Scope *savedScope = currentScope;
    currentScope = &scope;
    PrecheckModule(module);
    currentScope = savedScope;
}

void AnalysisContext::PrecheckDecl(const Decl &declaration) {
    const CheckingDeclaration checking(*this, declaration);
    if (const auto *structDecl = AstCast<const StructDecl>(&declaration)) {
        CheckStructDecl(*structDecl);
    }
    else if (const auto *enumDecl = AstCast<const EnumDecl>(&declaration)) {
        CheckEnumDecl(*enumDecl);
    }
    else if (const auto *unionDecl = AstCast<const UnionDecl>(&declaration)) {
        CheckUnionDecl(*unionDecl);
    }
    else if (const auto *constDecl = AstCast<const ConstDecl>(&declaration)) {
        if (checkingDeclarationsOnly)
            return;
        (void)CheckNamedConstant(*constDecl);
    }
    else if (const auto *globalDecl = AstCast<const GlobalDecl>(&declaration)) {
        CheckGlobalDecl(*globalDecl);
    }
    else if (const auto *implDecl = AstCast<const ImplDecl>(&declaration)) {
        for (const auto &constant : implDecl->constants) {
            (void)CheckAssociatedConstant(*constant);
            precheckedDeclarations.insert(constant.get());
        }
        return;
    }
    else if (const auto *moduleDecl = AstCast<const ModuleDecl>(&declaration)) {
        Scope *savedScope = currentScope;
        currentScope = &ModuleScopeFor(moduleDecl->name, *currentScope);
        for (const auto &item : moduleDecl->items) {
            PrecheckDecl(*item);
        }
        currentScope = savedScope;
        return;
    }
    else {
        return;
    }
    precheckedDeclarations.insert(&declaration);
}

void AnalysisContext::CheckModuleInScope(const Module &module, Scope &scope) {
    Scope *savedScope = currentScope;
    currentScope = &scope;
    CheckModule(module);
    currentScope = savedScope;
}
} // namespace Rux::SemanticDetail
