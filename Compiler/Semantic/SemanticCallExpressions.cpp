#include "Semantic/Detail/SemanticAnalyzerContext.h"

#include <algorithm>
#include <format>

namespace Rux::SemanticDetail {
TypeRef SemanticAnalyzerContext::CheckCallExpression(const CallExpr &expression) {
    const CallExpr *e = &expression;
    if (auto *ident = dynamic_cast<const IdentExpr *>(e->callee.get())) {
        std::vector<TypeRef> argTypes;
        argTypes.reserve(e->args.size());
        for (const auto &arg : e->args) {
            argTypes.push_back(CheckExpr(*arg));
        }

        // `#Error`/`#Warn` emit a diagnostic here rather than resolving as
        // an ordinary call; they contribute no value and no runtime code.
        if (Symbol *sym = currentScope->Lookup(ident->name);
            sym && (sym->intrinsicName == "#Error" || sym->intrinsicName == "#Warn")) {
            EmitDiagnosticIntrinsic(sym->intrinsicName, *e);
            return TypeRef::MakeOpaque();
        }

        if (Symbol *sym = currentScope->Lookup(ident->name);
            sym && sym->kind == Symbol::Kind::Func && !sym->funcOverloads.empty()) {
            const FuncDecl *decl = LookupFunctionOverload(*sym, argTypes, e->typeArgs);
            if (!decl) {
                std::string argList;
                for (std::size_t i = 0; i < argTypes.size(); ++i) {
                    if (i > 0) {
                        argList += ", ";
                    }
                    argList += argTypes[i].ToString();
                }
                EmitError(e->location, std::format("no matching overload for '{}' "
                                                   "with argument types ({})",
                                                   ident->name, argList));
                return TypeRef::MakeUnknown();
            }
            if (e->typeArgs.size() != decl->typeParams.size()) {
                EmitError(e->location, std::format("function '{}' expects {} type argument(s), got {}", ident->name,
                                                   decl->typeParams.size(), e->typeArgs.size()));
            }
            if (!decl->warnMessage.empty()) {
                EmitWarning(e->location, decl->warnMessage);
            }
            if (!decl->errorMessage.empty()) {
                EmitError(e->location, decl->errorMessage);
            }
            std::unordered_map<std::string, TypeRef> substitutions;
            const std::size_t count = std::min(decl->typeParams.size(), e->typeArgs.size());
            for (std::size_t i = 0; i < count; ++i) {
                substitutions.emplace(decl->typeParams[i], ResolveType(*e->typeArgs[i]));
            }
            QueueGenericInstantiation(*decl, substitutions);
            TypeRef funcType =
                MakeFuncTypeWithSubstitution(decl->params, decl->returnType, substitutions, decl->typeParams);
            const std::size_t paramCount =
                funcType.kind == TypeRef::Kind::Func && !funcType.inner.empty() ? funcType.inner.size() - 1 : 0;
            const bool isVariadic = !decl->params.empty() && decl->params.back().isVariadic;
            std::size_t requiredCount = 0;
            for (const auto &p : decl->params) {
                if (!p.isVariadic && !p.defaultValue) {
                    ++requiredCount;
                }
            }
            const bool arityOk = isVariadic ? argTypes.size() >= requiredCount
                                            : (argTypes.size() >= requiredCount && argTypes.size() <= paramCount);
            if (!arityOk) {
                EmitError(e->location,
                          std::format("function expects {} argument(s), got {}", paramCount, argTypes.size()));
            }
            else {
                for (std::size_t i = 0; i < argTypes.size() && i < paramCount; ++i) {
                    const TypeRef &paramType = funcType.inner[i];
                    if (!argTypes[i].IsUnknown() && !paramType.IsUnknown() &&
                        !CanAssignExprTo(*e->args[i], argTypes[i], paramType)) {
                        EmitError(e->args[i]->location, std::format("cannot pass '{}' to "
                                                                    "parameter of type '{}'",
                                                                    argTypes[i].ToString(), paramType.ToString()));
                    }
                }
                if (isVariadic) {
                    const TypeRef varElemType = ResolveType(*decl->params.back().type);
                    const TypeRef sliceType = TypeRef::MakeNamed(SliceTypeName(varElemType));
                    const bool isSingleSpread = (argTypes.size() == paramCount + 1 &&
                                                 dynamic_cast<const SpreadExpr *>(e->args[paramCount].get()));
                    if (isSingleSpread) {
                        if (!argTypes[paramCount].IsUnknown() && !sliceType.IsUnknown() &&
                            argTypes[paramCount] != sliceType) {
                            EmitError(e->args[paramCount]->location,
                                      std::format("cannot spread '{}' to "
                                                  "variadic "
                                                  "parameter of type '{}'",
                                                  argTypes[paramCount].ToString(), varElemType.ToString()));
                        }
                    }
                    else {
                        for (std::size_t i = paramCount; i < argTypes.size(); ++i) {
                            if (dynamic_cast<const SpreadExpr *>(e->args[i].get())) {
                                EmitError(e->args[i]->location, "spread argument must be "
                                                                "the only variadic "
                                                                "argument");
                            }
                            else if (!argTypes[i].IsUnknown() && !varElemType.IsUnknown() &&
                                     !CanAssignExprTo(*e->args[i], argTypes[i], varElemType)) {
                                EmitError(e->args[i]->location,
                                          std::format("cannot pass '{}' to "
                                                      "variadic "
                                                      "parameter of type '{}'",
                                                      argTypes[i].ToString(), varElemType.ToString()));
                            }
                        }
                    }
                }
            }
            RecordFunctionBinding(*e, *decl, ResolvedCallableBinding::DispatchKind::Direct, substitutions);
            return funcType.inner.empty() ? TypeRef::MakeUnknown() : funcType.inner.back();
        }
    }

    if (auto *field = dynamic_cast<const FieldExpr *>(e->callee.get())) {
        TypeRef receiverType = CheckExpr(*field->object);
        std::vector<TypeRef> argTypes;
        argTypes.reserve(e->args.size());
        for (const auto &arg : e->args) {
            argTypes.push_back(CheckExpr(*arg));
        }
        if (const FuncDecl *method = LookupMethod(receiverType, field->field, argTypes)) {
            if (!method->warnMessage.empty()) {
                EmitWarning(e->location, method->warnMessage);
            }
            if (!method->errorMessage.empty()) {
                EmitError(e->location, method->errorMessage);
            }
            std::vector<TypeRef> paramTypes = ResolveMethodParamTypes(receiverType, *method);

            if (argTypes.size() != paramTypes.size()) {
                EmitError(e->location,
                          std::format("function expects {} argument(s), got {}", paramTypes.size(), argTypes.size()));
            }
            else {
                for (std::size_t i = 0; i < argTypes.size(); ++i) {
                    const TypeRef &argType = argTypes[i];
                    const TypeRef &paramType = paramTypes[i];
                    if (!argType.IsUnknown() && !paramType.IsUnknown() &&
                        !CanAssignExprTo(*e->args[i], argType, paramType)) {
                        EmitError(e->args[i]->location, std::format("cannot pass '{}' to "
                                                                    "parameter of type '{}'",
                                                                    argType.ToString(), paramType.ToString()));
                    }
                }
            }

            RecordFunctionBinding(*e, *method, ResolvedCallableBinding::DispatchKind::Method,
                                  MethodTypeSubstitutions(receiverType), receiverType);
            return ResolveMethodReturnType(receiverType, *method);
        }

        if (const FuncDecl *method = LookupInterfaceMethod(receiverType, field->field)) {
            std::vector<TypeRef> paramTypes = ResolveInterfaceMethodParamTypes(*method);
            const bool isVariadic = !method->params.empty() && method->params.back().isVariadic;
            const bool arityOk =
                isVariadic ? argTypes.size() >= paramTypes.size() : argTypes.size() == paramTypes.size();

            if (!arityOk) {
                EmitError(e->location,
                          std::format("function expects {} argument(s), got {}", paramTypes.size(), argTypes.size()));
            }
            else {
                for (std::size_t i = 0; i < paramTypes.size(); ++i) {
                    const TypeRef &argType = argTypes[i];
                    const TypeRef &paramType = paramTypes[i];
                    if (!argType.IsUnknown() && !paramType.IsUnknown() &&
                        !CanAssignExprTo(*e->args[i], argType, paramType)) {
                        EmitError(e->args[i]->location, std::format("cannot pass '{}' to "
                                                                    "parameter of type '{}'",
                                                                    argType.ToString(), paramType.ToString()));
                    }
                }

                if (isVariadic) {
                    const TypeRef varElemType = ResolveType(*method->params.back().type);
                    for (std::size_t i = paramTypes.size(); i < argTypes.size(); ++i) {
                        if (!argTypes[i].IsUnknown() && !varElemType.IsUnknown() &&
                            !CanAssignExprTo(*e->args[i], argTypes[i], varElemType)) {
                            EmitError(e->args[i]->location,
                                      std::format("cannot pass '{}' to variadic "
                                                  "parameter of type '{}'",
                                                  argTypes[i].ToString(), varElemType.ToString()));
                        }
                    }
                }
            }

            RecordFunctionBinding(*e, *method, ResolvedCallableBinding::DispatchKind::Interface, {}, receiverType);
            return ResolveInterfaceMethodReturnType(*method);
        }
    }

    if (auto *path = dynamic_cast<const PathExpr *>(e->callee.get())) {
        if (path->segments.size() == 2) {
            Symbol *first = currentScope->Lookup(path->segments[0]);
            if (first && (first->kind == Symbol::Kind::Type || first->kind == Symbol::Kind::Interface)) {
                if (const auto enumIt = enumDecls.find(path->segments[0]); enumIt != enumDecls.end()) {
                    if (const EnumDecl::Variant *variant = LookupEnumVariant(path->segments[0], path->segments[1])) {
                        const EnumDecl &decl = *enumIt->second;
                        if (e->typeArgs.size() != decl.typeParams.size()) {
                            EmitError(e->location,
                                      std::format("enum variant '{}::{}' expects {} type argument(s), got {}",
                                                  path->segments[0], path->segments[1], decl.typeParams.size(),
                                                  e->typeArgs.size()));
                        }
                        std::vector<TypeRef> typeArgs;
                        typeArgs.reserve(e->typeArgs.size());
                        for (const auto &typeArg : e->typeArgs) {
                            typeArgs.push_back(ResolveType(*typeArg));
                        }
                        const TypeRef constructor = EnumVariantConstructorType(decl, *variant, typeArgs);
                        const std::size_t paramCount = constructor.inner.empty() ? 0 : constructor.inner.size() - 1;
                        std::vector<TypeRef> argTypes;
                        argTypes.reserve(e->args.size());
                        for (const auto &arg : e->args) {
                            argTypes.push_back(CheckExpr(*arg));
                        }
                        if (argTypes.size() != paramCount) {
                            EmitError(e->location, std::format("function expects {} argument(s), got {}", paramCount,
                                                               argTypes.size()));
                        }
                        else {
                            for (std::size_t i = 0; i < argTypes.size(); ++i) {
                                const TypeRef &paramType = constructor.inner[i];
                                if (!argTypes[i].IsUnknown() && !paramType.IsUnknown() &&
                                    !CanAssignExprTo(*e->args[i], argTypes[i], paramType)) {
                                    EmitError(e->args[i]->location,
                                              std::format("cannot pass '{}' to parameter of type '{}'",
                                                          argTypes[i].ToString(), paramType.ToString()));
                                }
                            }
                        }
                        ResolvedCallableBinding binding;
                        binding.dispatch = ResolvedCallableBinding::DispatchKind::EnumVariant;
                        binding.selectedDeclaration = &decl;
                        binding.selectedVariant = variant;
                        const std::size_t substitutionCount = std::min(decl.typeParams.size(), typeArgs.size());
                        for (std::size_t i = 0; i < substitutionCount; ++i) {
                            binding.substitutions.emplace(decl.typeParams[i], typeArgs[i]);
                        }
                        callableBindings.insert_or_assign(e, std::move(binding));
                        return constructor.inner.empty() ? TypeRef::MakeUnknown() : constructor.inner.back();
                    }
                }
                TypeRef receiverType = first->type.IsUnknown() ? TypeRef::MakeNamed(first->name) : first->type;
                if (const auto structIt = structDecls.find(path->segments[0]);
                    structIt != structDecls.end() && !structIt->second->typeParams.empty() &&
                    e->typeArgs.size() != structIt->second->typeParams.size()) {
                    EmitError(e->location,
                              std::format("associated function on '{}' expects {} type argument(s), got {}",
                                          path->segments[0], structIt->second->typeParams.size(), e->typeArgs.size()));
                }
                receiverType = InstantiateAssociatedReceiver(std::move(receiverType), e->typeArgs);
                const std::string &methodName = path->segments[1];
                std::vector<TypeRef> argTypes;
                argTypes.reserve(e->args.size());
                for (const auto &arg : e->args) {
                    argTypes.push_back(CheckExpr(*arg));
                }
                if (const FuncDecl *method = LookupMethod(receiverType, methodName, argTypes)) {
                    std::vector<TypeRef> paramTypes = ResolveMethodParamTypes(receiverType, *method);
                    if (argTypes.size() != paramTypes.size()) {
                        EmitError(e->location, std::format("function expects {} "
                                                           "argument(s), got {}",
                                                           paramTypes.size(), argTypes.size()));
                    }
                    else {
                        for (std::size_t i = 0; i < argTypes.size(); ++i) {
                            const TypeRef &argType = argTypes[i];
                            const TypeRef &paramType = paramTypes[i];
                            if (!argType.IsUnknown() && !paramType.IsUnknown() &&
                                !CanAssignExprTo(*e->args[i], argType, paramType)) {
                                EmitError(e->args[i]->location, std::format("cannot pass '{}' to "
                                                                            "parameter of type '{}'",
                                                                            argType.ToString(), paramType.ToString()));
                            }
                        }
                    }
                    RecordFunctionBinding(*e, *method, ResolvedCallableBinding::DispatchKind::Method,
                                          MethodTypeSubstitutions(receiverType), receiverType);
                    return ResolveMethodReturnType(receiverType, *method);
                }
            }
        }
    }

    Symbol *calleeSymbol = LookupCalleeSymbol(*e->callee);
    if (calleeSymbol && calleeSymbol->externDecl) {
        EmitCallSiteDiagnostics(*calleeSymbol->externDecl, e->location);
    }

    TypeRef calleeType = CheckExpr(*e->callee);
    std::vector<TypeRef> argTypes;
    argTypes.reserve(e->args.size());
    for (const auto &arg : e->args) {
        argTypes.push_back(CheckExpr(*arg));
    }

    if (calleeType.kind == TypeRef::Kind::Func && !calleeType.inner.empty()) {
        const std::size_t paramCount = calleeType.inner.size() - 1;
        const bool arityOk = calleeType.isVariadic ? argTypes.size() >= paramCount : argTypes.size() == paramCount;
        if (!arityOk) {
            EmitError(e->location, std::format("function expects {}{} argument(s), got {}",
                                               calleeType.isVariadic ? "at least " : "", paramCount, argTypes.size()));
        }
        else {
            // Only the fixed parameters are type-checked; trailing
            // C-variadic arguments accept any type.
            for (std::size_t i = 0; i < paramCount; ++i) {
                const TypeRef &argType = argTypes[i];
                const TypeRef &paramType = calleeType.inner[i];
                if (!argType.IsUnknown() && !paramType.IsUnknown() &&
                    !CanAssignExprTo(*e->args[i], argType, paramType)) {
                    EmitError(e->args[i]->location, std::format("cannot pass '{}' to "
                                                                "parameter of type '{}'",
                                                                argType.ToString(), paramType.ToString()));
                }
            }
        }
        if (calleeSymbol && calleeSymbol->externDecl) {
            RecordExternBinding(*e, *calleeSymbol->externDecl);
        }
        else if (calleeSymbol && !calleeSymbol->funcOverloads.empty()) {
            if (const FuncDecl *decl = LookupFunctionOverload(*calleeSymbol, argTypes, e->typeArgs)) {
                std::unordered_map<std::string, TypeRef> substitutions;
                const std::size_t count = std::min(decl->typeParams.size(), e->typeArgs.size());
                for (std::size_t i = 0; i < count; ++i) {
                    substitutions.emplace(decl->typeParams[i], ResolveType(*e->typeArgs[i]));
                }
                RecordFunctionBinding(*e, *decl, ResolvedCallableBinding::DispatchKind::Direct,
                                      std::move(substitutions));
            }
        }
        else {
            ResolvedCallableBinding binding;
            binding.dispatch = ResolvedCallableBinding::DispatchKind::Indirect;
            if (calleeType.isVariadic) {
                binding.variadicBoundary = paramCount;
            }
            callableBindings.insert_or_assign(e, std::move(binding));
        }
        return calleeType.inner.back();
    }
    return TypeRef::MakeUnknown();
}

// Resolves a direct or module-qualified callee without emitting diagnostics.
// Expression checking remains responsible for invalid names and paths; this
// lookup only recovers declaration metadata absent from a function TypeRef.
Symbol *SemanticAnalyzerContext::LookupCalleeSymbol(const Expr &callee) const {
    if (const auto *identifier = dynamic_cast<const IdentExpr *>(&callee)) {
        return currentScope->Lookup(identifier->name);
    }

    const auto *path = dynamic_cast<const PathExpr *>(&callee);
    if (!path || path->segments.empty()) {
        return nullptr;
    }

    Symbol *current = currentScope->Lookup(path->segments[0]);
    for (std::size_t i = 1; current && i < path->segments.size(); ++i) {
        if (current->kind != Symbol::Kind::Module || !current->moduleScope) {
            return nullptr;
        }
        current = current->moduleScope->Lookup(path->segments[i]);
    }
    return current;
}

void SemanticAnalyzerContext::EmitCallSiteDiagnostics(const Decl &declaration, const SourceLocation location) const {
    if (!declaration.warnMessage.empty()) {
        EmitWarning(location, declaration.warnMessage);
    }
    if (!declaration.errorMessage.empty()) {
        EmitError(location, declaration.errorMessage);
    }
}

void SemanticAnalyzerContext::RecordFunctionBinding(const CallExpr &call, const FuncDecl &declaration,
                                                    const ResolvedCallableBinding::DispatchKind dispatch,
                                                    std::unordered_map<std::string, TypeRef> substitutions,
                                                    std::optional<TypeRef> receiverType) {
    ResolvedCallableBinding binding;
    binding.dispatch = dispatch;
    binding.selectedDeclaration = &declaration;
    binding.substitutions = std::move(substitutions);
    binding.receiverType = std::move(receiverType);
    binding.callingConvention = declaration.callConv;
    if (!declaration.params.empty() && declaration.params.back().isVariadic) {
        std::size_t boundary = 0;
        for (const auto &parameter : declaration.params) {
            if (parameter.isVariadic) {
                break;
            }
            if (dispatch != ResolvedCallableBinding::DispatchKind::Method || parameter.name != "self") {
                ++boundary;
            }
        }
        binding.variadicBoundary = boundary;
    }
    callableBindings.insert_or_assign(&call, std::move(binding));
}

void SemanticAnalyzerContext::RecordExternBinding(const CallExpr &call, const ExternFuncDecl &declaration) {
    ResolvedCallableBinding binding;
    binding.dispatch = ResolvedCallableBinding::DispatchKind::Direct;
    binding.selectedDeclaration = &declaration;
    binding.callingConvention = declaration.callConv;
    binding.importedSymbolOverride = declaration.symbolName;
    if (declaration.isVariadic) {
        binding.variadicBoundary = declaration.params.size();
    }
    callableBindings.insert_or_assign(&call, std::move(binding));
}
} // namespace Rux::SemanticDetail
