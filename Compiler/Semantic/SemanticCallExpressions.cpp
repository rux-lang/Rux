// Call checking: overload resolution, argument compatibility, and recording the
// binding lowering will later emit without re-resolving.

#include "Semantic/Detail/SemanticAnalyzerContext.h"
#include "Syntax/Parser/Detail/AstDumpWriter.h"

#include <algorithm>
#include <format>

namespace Rux::SemanticDetail {
namespace {
using ParserDumpDetail::DeclarationPrinter;

[[nodiscard]] std::string ArgumentTypes(const std::vector<TypeRef> &types) {
    std::string result;
    for (std::size_t index = 0; index < types.size(); ++index) {
        if (index != 0) {
            result += ", ";
        }
        result += types[index].ToString();
    }
    return result;
}

[[nodiscard]] std::vector<const Param *> VisibleParameters(const FuncDecl &declaration, const bool isMethod) {
    std::vector<const Param *> result;
    for (const Param &parameter : declaration.params) {
        if (isMethod && parameter.name == "self") {
            continue;
        }
        result.push_back(&parameter);
    }
    return result;
}

[[nodiscard]] std::string CandidateSignature(const FuncDecl &declaration, const bool isMethod) {
    std::string result = declaration.name + "(";
    const auto parameters = VisibleParameters(declaration, isMethod);
    for (std::size_t index = 0; index < parameters.size(); ++index) {
        if (index != 0) {
            result += ", ";
        }
        const Param &parameter = *parameters[index];
        result += parameter.name;
        result += ": ";
        result += DeclarationPrinter::TypeString(parameter.type.get());
        if (parameter.isVariadic) {
            result += "...";
        }
        if (parameter.defaultValue) {
            result += " = ...";
        }
    }
    return result + ")";
}

[[nodiscard]] std::string Counted(const std::size_t count, const std::string_view singular) {
    return std::format("{} {}{}", count, singular, count == 1 ? "" : "s");
}
} // namespace

TypeRef SemanticAnalyzerContext::CheckCallExpression(const CallExpr &expression) {
    const CallExpr *e = &expression;
    const auto sourceFor = [&](const FuncDecl &declaration) -> std::string {
        if (const auto source = functionDeclFiles.find(&declaration); source != functionDeclFiles.end()) {
            return source->second;
        }
        return currentFile;
    };
    const auto declarationNote = [&](const FuncDecl &declaration, const bool isMethod) {
        return std::format("candidate '{}' declared at '{}':{}:{}", CandidateSignature(declaration, isMethod),
                           sourceFor(declaration), declaration.location.line, declaration.location.column);
    };
    const auto parameterNote = [&](const Param &parameter, const FuncDecl &declaration) {
        return std::format("parameter '{}' declared at '{}':{}:{}", parameter.name, sourceFor(declaration),
                           parameter.location.line, parameter.location.column);
    };
    const auto emitArityError = [&](const std::string_view callable, const std::size_t requiredCount,
                                    const std::size_t maximumCount, const bool variadic, const std::size_t actualCount,
                                    const FuncDecl *declaration = nullptr, const bool isMethod = false) {
        std::string expectation;
        if (variadic) {
            expectation = "at least " + Counted(requiredCount, "argument");
        }
        else if (requiredCount != maximumCount) {
            expectation = std::format("between {} and {} arguments", requiredCount, maximumCount);
        }
        else {
            expectation = Counted(maximumCount, "argument");
        }
        std::vector<std::string> notes;
        if (declaration) {
            notes.push_back(declarationNote(*declaration, isMethod));
        }
        EmitError(e->location,
                  std::format("call to '{}' expects {}, but {} provided", callable, expectation,
                              actualCount == 1 ? "1 was" : std::format("{} were", actualCount)),
                  std::move(notes));
    };
    const auto emitArgumentTypeError = [&](const std::string_view callable, const std::size_t argumentIndex,
                                           const TypeRef &argumentType, const TypeRef &parameterType,
                                           const Param *parameter, const FuncDecl *declaration = nullptr,
                                           const bool variadic = false) {
        const std::string parameterName = parameter && !parameter->name.empty()
                                            ? std::format("parameter '{}'", parameter->name)
                                            : std::format("parameter {}", argumentIndex + 1);
        std::vector<std::string> notes;
        if (parameter && declaration) {
            notes.push_back(parameterNote(*parameter, *declaration));
        }
        EmitError(e->args[argumentIndex]->location,
                  std::format("argument {} to '{}' has type '{}', but {}{} requires '{}'", argumentIndex + 1, callable,
                              argumentType.ToString(), variadic ? "variadic " : "", parameterName,
                              parameterType.ToString()),
                  std::move(notes));
    };
    const auto emitOverloadError = [&](const std::string_view callable, const std::vector<TypeRef> &argumentTypes,
                                       const std::vector<const FuncDecl *> &candidates, const bool isMethod = false,
                                       const std::optional<TypeRef> &receiverType = std::nullopt) {
        std::vector<std::string> notes;
        notes.reserve(candidates.size());
        for (const FuncDecl *candidate : candidates) {
            notes.push_back(declarationNote(*candidate, isMethod));
        }
        const std::string subject = receiverType
                                      ? std::format("method '{}' on type '{}'", callable, receiverType->ToString())
                                      : std::format("'{}'", callable);
        EmitError(
            e->location,
            std::format("no matching overload for {} with argument types ({})", subject, ArgumentTypes(argumentTypes)),
            std::move(notes));
    };
    if (auto *ident = dynamic_cast<const IdentExpr *>(e->callee.get())) {
        const std::vector<TypeRef> argTypes = CheckCallArgumentValues(*e);

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
                if (sym->funcOverloads.size() != 1) {
                    emitOverloadError(ident->name, argTypes, sym->funcOverloads);
                    return TypeRef::MakeUnknown();
                }

                const FuncDecl &candidate = *sym->funcOverloads.front();
                std::unordered_map<std::string, TypeRef> candidateSubstitutions;
                const std::size_t substitutionCount = std::min(candidate.typeParams.size(), e->typeArgs.size());
                for (std::size_t index = 0; index < substitutionCount; ++index) {
                    candidateSubstitutions.emplace(candidate.typeParams[index].name, ResolveType(*e->typeArgs[index]));
                }
                const TypeRef candidateType =
                    MakeFuncTypeWithSubstitution(candidate.params, candidate.returnType, candidateSubstitutions,
                                                 TypeParameterNames(candidate.typeParams));
                const std::size_t candidateParamCount =
                    candidateType.inner.empty() ? 0 : candidateType.inner.size() - 1;
                std::size_t candidateRequiredCount = 0;
                for (const Param &parameter : candidate.params) {
                    if (!parameter.isVariadic && !parameter.defaultValue) {
                        ++candidateRequiredCount;
                    }
                }
                const bool candidateIsVariadic = !candidate.params.empty() && candidate.params.back().isVariadic;
                const bool candidateArityOk = candidateIsVariadic ? argTypes.size() >= candidateRequiredCount
                                                                  : argTypes.size() >= candidateRequiredCount &&
                                                                        argTypes.size() <= candidateParamCount;
                if (!candidateArityOk) {
                    emitArityError(ident->name, candidateRequiredCount, candidateParamCount, candidateIsVariadic,
                                   argTypes.size(), &candidate);
                    return TypeRef::MakeUnknown();
                }

                const auto parameters = VisibleParameters(candidate, false);
                for (std::size_t index = 0; index < std::min(argTypes.size(), candidateParamCount); ++index) {
                    const TypeRef &parameterType = candidateType.inner[index];
                    if (!argTypes[index].IsUnknown() && !parameterType.IsUnknown() &&
                        !CanAssignExprTo(*e->args[index], argTypes[index], parameterType)) {
                        emitArgumentTypeError(ident->name, index, argTypes[index], parameterType,
                                              index < parameters.size() ? parameters[index] : nullptr, &candidate);
                        return TypeRef::MakeUnknown();
                    }
                }

                emitOverloadError(ident->name, argTypes, sym->funcOverloads);
                return TypeRef::MakeUnknown();
            }
            bool callAccepted = e->typeArgs.size() == decl->typeParams.size();
            if (!callAccepted) {
                EmitError(e->location,
                          std::format("function '{}' requires {}, but {} provided", ident->name,
                                      Counted(decl->typeParams.size(), "type argument"),
                                      e->typeArgs.size() == 1 ? "1 was" : std::format("{} were", e->typeArgs.size())),
                          {declarationNote(*decl, false)});
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
                substitutions.emplace(decl->typeParams[i].name, ResolveType(*e->typeArgs[i]));
            }
            CheckTypeArgumentConstraints(decl->typeParams, substitutions, e->location,
                                         std::format("function '{}'", ident->name));
            QueueGenericInstantiation(*decl, substitutions);
            TypeRef funcType = MakeFuncTypeWithSubstitution(decl->params, decl->returnType, substitutions,
                                                            TypeParameterNames(decl->typeParams));
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
                callAccepted = false;
                emitArityError(ident->name, requiredCount, paramCount, isVariadic, argTypes.size(), decl);
            }
            else {
                const auto parameters = VisibleParameters(*decl, false);
                for (std::size_t i = 0; i < argTypes.size() && i < paramCount; ++i) {
                    const TypeRef &paramType = funcType.inner[i];
                    if (!argTypes[i].IsUnknown() && !paramType.IsUnknown() &&
                        !CanAssignExprTo(*e->args[i], argTypes[i], paramType)) {
                        callAccepted = false;
                        emitArgumentTypeError(ident->name, i, argTypes[i], paramType,
                                              i < parameters.size() ? parameters[i] : nullptr, decl);
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
                            callAccepted = false;
                            emitArgumentTypeError(ident->name, paramCount, argTypes[paramCount], sliceType,
                                                  &decl->params.back(), decl, true);
                        }
                    }
                    else {
                        for (std::size_t i = paramCount; i < argTypes.size(); ++i) {
                            if (dynamic_cast<const SpreadExpr *>(e->args[i].get())) {
                                callAccepted = false;
                                EmitError(e->args[i]->location,
                                          std::format("spread argument to '{}' must be the only argument for variadic "
                                                      "parameter '{}'",
                                                      ident->name, decl->params.back().name),
                                          {parameterNote(decl->params.back(), *decl)});
                            }
                            else if (!argTypes[i].IsUnknown() && !varElemType.IsUnknown() &&
                                     !CanAssignExprTo(*e->args[i], argTypes[i], varElemType)) {
                                callAccepted = false;
                                emitArgumentTypeError(ident->name, i, argTypes[i], varElemType, &decl->params.back(),
                                                      decl, true);
                            }
                        }
                    }
                }
            }
            if (callAccepted) {
                ConsumeCallArguments(*e, argTypes);
            }
            RecordFunctionBinding(*e, *decl, ResolvedCallableBinding::DispatchKind::Direct, substitutions);
            return funcType.inner.empty() ? TypeRef::MakeUnknown() : funcType.inner.back();
        }
    }

    if (auto *field = dynamic_cast<const FieldExpr *>(e->callee.get())) {
        TypeRef receiverType = CheckExpr(*field->object);
        const std::vector<TypeRef> argTypes = CheckCallArgumentValues(*e);

        // A receiver whose type is a generic parameter carries exactly the operations its bounds declare. The concrete
        // method is chosen per instantiation, so the call is checked against the interface's signature here and the
        // witness recorded for each type argument is what lowering calls.
        const TypeRef &receiverBase = receiverType.kind == TypeRef::Kind::Pointer && !receiverType.inner.empty()
                                        ? receiverType.inner.front()
                                        : receiverType;
        if (receiverBase.kind == TypeRef::Kind::TypeParam) {
            const auto constrained = LookupConstrainedOperation(receiverType, field->field);
            if (!constrained) {
                EmitMissingConstrainedOperation(field->location, receiverType, field->field);
                return TypeRef::MakeUnknown();
            }
            const FuncDecl &operation = *constrained->operation;
            const std::vector<TypeRef> paramTypes = ResolveInterfaceMethodParamTypes(operation, receiverBase);
            bool callAccepted = argTypes.size() == paramTypes.size();
            if (!callAccepted) {
                emitArityError(field->field, paramTypes.size(), paramTypes.size(), false, argTypes.size(), &operation,
                               true);
            }
            else {
                const auto parameters = VisibleParameters(operation, true);
                for (std::size_t i = 0; i < paramTypes.size(); ++i) {
                    if (!argTypes[i].IsUnknown() && !paramTypes[i].IsUnknown() &&
                        !CanAssignExprTo(*e->args[i], argTypes[i], paramTypes[i])) {
                        callAccepted = false;
                        emitArgumentTypeError(field->field, i, argTypes[i], paramTypes[i],
                                              i < parameters.size() ? parameters[i] : nullptr, &operation);
                    }
                }
            }
            if (callAccepted) {
                ConsumeCallArguments(*e, argTypes);
            }
            RecordConstrainedBinding(*e, *constrained, receiverType);
            return ResolveInterfaceMethodReturnType(operation, receiverBase);
        }

        if (const FuncDecl *method = LookupMethod(receiverType, field->field, argTypes)) {
            bool callAccepted = true;
            if (!method->warnMessage.empty()) {
                EmitWarning(e->location, method->warnMessage);
            }
            if (!method->errorMessage.empty()) {
                EmitError(e->location, method->errorMessage);
            }
            std::vector<TypeRef> paramTypes = ResolveMethodParamTypes(receiverType, *method);
            // A method of a generic type is an instantiation like any other: its body was checked with the type's
            // parameters standing for nothing in particular, and the questions that could not be answered then --
            // above all whether handing a `T` over consumes it -- are answered here, where the receiver says what
            // the parameters stand for.
            QueueGenericInstantiation(*method, MethodTypeSubstitutions(receiverType));

            if (argTypes.size() != paramTypes.size()) {
                callAccepted = false;
                emitArityError(field->field, paramTypes.size(), paramTypes.size(), false, argTypes.size(), method,
                               true);
            }
            else {
                const auto parameters = VisibleParameters(*method, true);
                for (std::size_t i = 0; i < argTypes.size(); ++i) {
                    const TypeRef &argType = argTypes[i];
                    const TypeRef &paramType = paramTypes[i];
                    if (!argType.IsUnknown() && !paramType.IsUnknown() &&
                        !CanAssignExprTo(*e->args[i], argType, paramType)) {
                        callAccepted = false;
                        emitArgumentTypeError(field->field, i, argType, paramType,
                                              i < parameters.size() ? parameters[i] : nullptr, method);
                    }
                }
            }

            callAccepted = CheckReceiverMutability(*e, *field->object, receiverType, *method) && callAccepted;
            if (callAccepted) {
                ConsumeMethodReceiver(*e, *field->object, receiverType, *method);
                ConsumeCallArguments(*e, argTypes);
            }
            RecordFunctionBinding(*e, *method, ResolvedCallableBinding::DispatchKind::Method,
                                  MethodTypeSubstitutions(receiverType), receiverType);
            return ResolveMethodReturnType(receiverType, *method);
        }

        const std::string receiverName = NamedBaseTypeName(receiverType);
        if (const auto typeMethods = methodsByType.find(receiverName); typeMethods != methodsByType.end()) {
            if (const auto namedMethods = typeMethods->second.find(field->field);
                namedMethods != typeMethods->second.end() && !namedMethods->second.empty()) {
                const auto &candidates = namedMethods->second;
                if (candidates.size() == 1) {
                    const FuncDecl &candidate = *candidates.front();
                    const std::vector<TypeRef> parameterTypes = ResolveMethodParamTypes(receiverType, candidate);
                    if (argTypes.size() != parameterTypes.size()) {
                        emitArityError(field->field, parameterTypes.size(), parameterTypes.size(), false,
                                       argTypes.size(), &candidate, true);
                    }
                    else {
                        const auto parameters = VisibleParameters(candidate, true);
                        for (std::size_t index = 0; index < argTypes.size(); ++index) {
                            if (!argTypes[index].IsUnknown() && !parameterTypes[index].IsUnknown() &&
                                !CanAssignExprTo(*e->args[index], argTypes[index], parameterTypes[index])) {
                                emitArgumentTypeError(field->field, index, argTypes[index], parameterTypes[index],
                                                      index < parameters.size() ? parameters[index] : nullptr,
                                                      &candidate);
                                break;
                            }
                        }
                    }
                }
                else {
                    emitOverloadError(field->field, argTypes, candidates, true, receiverType);
                }
                return TypeRef::MakeUnknown();
            }
        }

        if (const FuncDecl *method = LookupInterfaceMethod(receiverType, field->field)) {
            // Through an interface value the implementing type is not known, so a method naming `Self` has no meaning
            // here: two values of the same interface may hold different types, and the call would pair them up.
            if (InterfaceMethodMentionsSelf(*method)) {
                EmitError(field->location,
                          std::format("method '{}' of interface '{}' names 'Self', so it can only be called where the "
                                      "implementing type is known",
                                      field->field, receiverType.ToString()),
                          {}, "call it through a generic parameter bounded by the interface");
                return TypeRef::MakeUnknown();
            }
            std::vector<TypeRef> paramTypes = ResolveInterfaceMethodParamTypes(*method, receiverType);
            const bool isVariadic = !method->params.empty() && method->params.back().isVariadic;
            const bool arityOk =
                isVariadic ? argTypes.size() >= paramTypes.size() : argTypes.size() == paramTypes.size();
            bool callAccepted = arityOk;

            if (!arityOk) {
                emitArityError(field->field, paramTypes.size(), paramTypes.size(), isVariadic, argTypes.size(), method,
                               true);
            }
            else {
                const auto parameters = VisibleParameters(*method, true);
                for (std::size_t i = 0; i < paramTypes.size(); ++i) {
                    const TypeRef &argType = argTypes[i];
                    const TypeRef &paramType = paramTypes[i];
                    if (!argType.IsUnknown() && !paramType.IsUnknown() &&
                        !CanAssignExprTo(*e->args[i], argType, paramType)) {
                        callAccepted = false;
                        emitArgumentTypeError(field->field, i, argType, paramType,
                                              i < parameters.size() ? parameters[i] : nullptr, method);
                    }
                }

                if (isVariadic) {
                    const TypeRef varElemType = ResolveType(*method->params.back().type);
                    for (std::size_t i = paramTypes.size(); i < argTypes.size(); ++i) {
                        if (!argTypes[i].IsUnknown() && !varElemType.IsUnknown() &&
                            !CanAssignExprTo(*e->args[i], argTypes[i], varElemType)) {
                            callAccepted = false;
                            emitArgumentTypeError(field->field, i, argTypes[i], varElemType, &method->params.back(),
                                                  method, true);
                        }
                    }
                }
            }

            if (callAccepted) {
                ConsumeCallArguments(*e, argTypes);
            }
            RecordFunctionBinding(*e, *method, ResolvedCallableBinding::DispatchKind::Interface, {}, receiverType);
            return ResolveInterfaceMethodReturnType(*method, receiverType);
        }
    }

    if (auto *path = dynamic_cast<const PathExpr *>(e->callee.get())) {
        if (path->segments.size() == 2) {
            Symbol *first = currentScope->Lookup(path->segments[0]);
            if (first && (first->kind == Symbol::Kind::Type || first->kind == Symbol::Kind::Interface)) {
                if (const EnumDecl *enumeration = EnumNamed(path->segments[0])) {
                    if (const EnumDecl::Variant *variant = LookupEnumVariant(path->segments[0], path->segments[1])) {
                        const EnumDecl &decl = *enumeration;
                        if (e->typeArgs.size() != decl.typeParams.size()) {
                            EmitError(e->location, std::format("enum variant '{}::{}' requires {}, but {} provided",
                                                               path->segments[0], path->segments[1],
                                                               Counted(decl.typeParams.size(), "type argument"),
                                                               e->typeArgs.size() == 1
                                                                   ? "1 was"
                                                                   : std::format("{} were", e->typeArgs.size())));
                        }
                        std::vector<TypeRef> typeArgs;
                        typeArgs.reserve(e->typeArgs.size());
                        for (const auto &typeArg : e->typeArgs) {
                            typeArgs.push_back(ResolveType(*typeArg));
                        }
                        const TypeRef constructor = EnumVariantConstructorType(decl, *variant, typeArgs);
                        const std::size_t paramCount = constructor.inner.empty() ? 0 : constructor.inner.size() - 1;
                        const std::vector<TypeRef> argTypes = CheckCallArgumentValues(*e);
                        bool callAccepted = argTypes.size() == paramCount;
                        if (argTypes.size() != paramCount) {
                            emitArityError(std::format("{}::{}", path->segments[0], path->segments[1]), paramCount,
                                           paramCount, false, argTypes.size());
                        }
                        else {
                            for (std::size_t i = 0; i < argTypes.size(); ++i) {
                                const TypeRef &paramType = constructor.inner[i];
                                if (!argTypes[i].IsUnknown() && !paramType.IsUnknown() &&
                                    !CanAssignExprTo(*e->args[i], argTypes[i], paramType)) {
                                    callAccepted = false;
                                    EmitError(e->args[i]->location,
                                              std::format("argument {} to enum variant '{}::{}' has type '{}', but "
                                                          "field {} requires '{}'",
                                                          i + 1, path->segments[0], path->segments[1],
                                                          argTypes[i].ToString(), i + 1, paramType.ToString()),
                                              {std::format("variant '{}::{}' declared at '{}':{}:{}", path->segments[0],
                                                           path->segments[1], currentFile, variant->location.line,
                                                           variant->location.column)});
                                }
                            }
                        }
                        if (callAccepted) {
                            ConsumeCallArguments(*e, argTypes);
                        }
                        ResolvedCallableBinding binding;
                        binding.dispatch = ResolvedCallableBinding::DispatchKind::EnumVariant;
                        binding.selectedDeclaration = &decl;
                        binding.selectedVariant = variant;
                        const std::size_t substitutionCount = std::min(decl.typeParams.size(), typeArgs.size());
                        for (std::size_t i = 0; i < substitutionCount; ++i) {
                            binding.substitutions.emplace(decl.typeParams[i].name, typeArgs[i]);
                        }
                        CheckTypeArgumentConstraints(decl.typeParams, binding.substitutions, e->location,
                                                     std::format("enum '{}'", decl.name));
                        callableBindings.insert_or_assign(e, std::move(binding));
                        return constructor.inner.empty() ? TypeRef::MakeUnknown() : constructor.inner.back();
                    }
                }
                TypeRef receiverType = first->type.IsUnknown() ? TypeRef::MakeNamed(first->name) : first->type;
                if (const auto structIt = structDecls.find(path->segments[0]);
                    structIt != structDecls.end() && !structIt->second->typeParams.empty() &&
                    e->typeArgs.size() != structIt->second->typeParams.size()) {
                    EmitError(
                        e->location,
                        std::format("associated function on '{}' requires {}, but {} provided", path->segments[0],
                                    Counted(structIt->second->typeParams.size(), "type argument"),
                                    e->typeArgs.size() == 1 ? "1 was" : std::format("{} were", e->typeArgs.size())));
                }
                else if (const auto generic = structDecls.find(path->segments[0]); generic != structDecls.end()) {
                    CheckWrittenTypeArgumentConstraints(generic->second->typeParams, e->typeArgs, e->location,
                                                        std::format("struct '{}'", path->segments[0]));
                }
                receiverType = InstantiateAssociatedReceiver(std::move(receiverType), e->typeArgs);
                const std::string &methodName = path->segments[1];
                const std::vector<TypeRef> argTypes = CheckCallArgumentValues(*e);
                if (const FuncDecl *method = LookupMethod(receiverType, methodName, argTypes)) {
                    std::vector<TypeRef> paramTypes = ResolveMethodParamTypes(receiverType, *method);
                    // An associated function on a generic type is instantiated by the type arguments written at the
                    // call, exactly as a method is by its receiver's.
                    QueueGenericInstantiation(*method, MethodTypeSubstitutions(receiverType));
                    bool callAccepted = argTypes.size() == paramTypes.size();
                    if (argTypes.size() != paramTypes.size()) {
                        emitArityError(std::format("{}::{}", path->segments[0], methodName), paramTypes.size(),
                                       paramTypes.size(), false, argTypes.size(), method, true);
                    }
                    else {
                        const auto parameters = VisibleParameters(*method, true);
                        for (std::size_t i = 0; i < argTypes.size(); ++i) {
                            const TypeRef &argType = argTypes[i];
                            const TypeRef &paramType = paramTypes[i];
                            if (!argType.IsUnknown() && !paramType.IsUnknown() &&
                                !CanAssignExprTo(*e->args[i], argType, paramType)) {
                                callAccepted = false;
                                emitArgumentTypeError(std::format("{}::{}", path->segments[0], methodName), i, argType,
                                                      paramType, i < parameters.size() ? parameters[i] : nullptr,
                                                      method);
                            }
                        }
                    }
                    if (callAccepted) {
                        ConsumeCallArguments(*e, argTypes);
                    }
                    RecordFunctionBinding(*e, *method, ResolvedCallableBinding::DispatchKind::Method,
                                          MethodTypeSubstitutions(receiverType), receiverType);
                    return ResolveMethodReturnType(receiverType, *method);
                }

                const std::string receiverName = NamedBaseTypeName(receiverType);
                if (const auto typeMethods = methodsByType.find(receiverName); typeMethods != methodsByType.end()) {
                    if (const auto namedMethods = typeMethods->second.find(methodName);
                        namedMethods != typeMethods->second.end() && !namedMethods->second.empty()) {
                        const std::string callable = std::format("{}::{}", path->segments[0], methodName);
                        const auto &candidates = namedMethods->second;
                        if (candidates.size() == 1) {
                            const FuncDecl &candidate = *candidates.front();
                            const auto parameterTypes = ResolveMethodParamTypes(receiverType, candidate);
                            if (argTypes.size() != parameterTypes.size()) {
                                emitArityError(callable, parameterTypes.size(), parameterTypes.size(), false,
                                               argTypes.size(), &candidate, true);
                            }
                            else {
                                const auto parameters = VisibleParameters(candidate, true);
                                for (std::size_t index = 0; index < argTypes.size(); ++index) {
                                    if (!argTypes[index].IsUnknown() && !parameterTypes[index].IsUnknown() &&
                                        !CanAssignExprTo(*e->args[index], argTypes[index], parameterTypes[index])) {
                                        emitArgumentTypeError(callable, index, argTypes[index], parameterTypes[index],
                                                              index < parameters.size() ? parameters[index] : nullptr,
                                                              &candidate);
                                        break;
                                    }
                                }
                            }
                        }
                        else {
                            emitOverloadError(callable, argTypes, candidates, true, receiverType);
                        }
                        return TypeRef::MakeUnknown();
                    }
                }
            }
        }
    }

    Symbol *calleeSymbol = LookupCalleeSymbol(*e->callee);
    if (calleeSymbol && calleeSymbol->externDecl) {
        EmitCallSiteDiagnostics(*calleeSymbol->externDecl, e->location);
    }

    TypeRef calleeType = CheckExpr(*e->callee);
    const std::vector<TypeRef> argTypes = CheckCallArgumentValues(*e);

    if (calleeType.kind == TypeRef::Kind::Func && !calleeType.inner.empty()) {
        const std::size_t paramCount = calleeType.inner.size() - 1;
        const bool arityOk = calleeType.isVariadic ? argTypes.size() >= paramCount : argTypes.size() == paramCount;
        std::string callableName = "function value";
        if (const auto *identifier = dynamic_cast<const IdentExpr *>(e->callee.get())) {
            callableName = identifier->name;
        }
        else if (const auto *path = dynamic_cast<const PathExpr *>(e->callee.get()); path && !path->segments.empty()) {
            callableName = path->segments.back();
        }
        if (!arityOk) {
            emitArityError(callableName, paramCount, paramCount, calleeType.isVariadic, argTypes.size());
        }
        else {
            bool callAccepted = true;
            // Only the fixed parameters are type-checked; trailing
            // C-variadic arguments accept any type.
            for (std::size_t i = 0; i < paramCount; ++i) {
                const TypeRef &argType = argTypes[i];
                const TypeRef &paramType = calleeType.inner[i];
                if (!argType.IsUnknown() && !paramType.IsUnknown() &&
                    !CanAssignExprTo(*e->args[i], argType, paramType)) {
                    callAccepted = false;
                    const Param *parameter = nullptr;
                    if (calleeSymbol && calleeSymbol->externDecl && i < calleeSymbol->externDecl->params.size()) {
                        parameter = &calleeSymbol->externDecl->params[i];
                    }
                    emitArgumentTypeError(callableName, i, argType, paramType, parameter);
                }
            }
            if (callAccepted) {
                ConsumeCallArguments(*e, argTypes);
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
                    substitutions.emplace(decl->typeParams[i].name, ResolveType(*e->typeArgs[i]));
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

/// Resolves a direct or module-qualified callee without emitting diagnostics. Expression checking remains responsible
/// for invalid names and paths; this lookup only recovers declaration metadata absent from a function TypeRef.
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
