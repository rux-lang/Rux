#include "Lexer/Lexer.h"
#include "Numeric/IntegerLiteral.h"
#include "Semantic/Analysis/AnalysisContext.h"
#include "Semantic/Conditional/ConditionalCompilation.h"
#include "Target/Layout.h"
#include "Target/Target.h"
#include "Types/Type.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <charconv>
#include <format>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace Rux::SemanticDetail {
using Layout::AlignUp;

std::unordered_map<std::string, TypeRef> AnalysisContext::MethodTypeSubstitutions(const TypeRef &receiverType) const {
    const TypeRef *receiver = &receiverType;
    if (((*receiver)->kind == TypeRef::Kind::Pointer || (*receiver)->kind == TypeRef::Kind::Reference) &&
        !(*receiver)->inner.empty()) {
        receiver = &(*receiver)->inner[0];
    }
    if ((*receiver)->kind != TypeRef::Kind::Named) {
        return {};
    }

    const std::vector<TypeParameter> *typeParams = AggregateTypeParams(BaseTypeName((*receiver)->name));
    if (!typeParams) {
        return {};
    }
    const std::vector<TypeRef> args = ParseTypeArgsFromTypeName((*receiver)->name);
    std::unordered_map<std::string, TypeRef> substitutions;
    const std::size_t count = std::min(typeParams->size(), args.size());
    for (std::size_t i = 0; i < count; ++i) {
        substitutions.emplace((*typeParams)[i].name, args[i]);
    }
    return substitutions;
}

TypeRef AnalysisContext::InstantiateAssociatedReceiver(TypeRef receiverType, const std::vector<TypeExprPtr> &typeArgs) {
    const std::string typeName = NamedBaseTypeName(receiverType);
    const std::vector<TypeParameter> *typeParams = AggregateTypeParams(typeName);
    if (!typeParams || typeParams->empty() || typeArgs.empty()) {
        return receiverType;
    }

    std::string name = typeName + "<";
    for (std::size_t i = 0; i < std::min(typeArgs.size(), typeParams->size()); ++i) {
        if (i) {
            name += ", ";
        }
        name += ResolveType(*typeArgs[i]).ToString();
    }
    name += ">";
    return TypeRef::MakeNamed(std::move(name));
}

TypeRef AnalysisContext::ResolveMethodReturnType(const TypeRef &receiverType, const FuncDecl &method,
                                                 const std::unordered_map<std::string, TypeRef> &methodSubstitutions) {
    const auto savedTypeParams = currentTypeParams;
    AppendTypeParameterNames(currentTypeParams, method.typeParams);
    TypeRef savedSelfType = currentSelfType;
    currentSelfType = receiverType->kind == TypeRef::Kind::Pointer || receiverType->kind == TypeRef::Kind::Reference
                        ? receiverType
                        : TypeRef::MakePointer(receiverType);
    auto substitutions = MethodTypeSubstitutions(receiverType);
    for (const auto &[name, type] : methodSubstitutions) {
        substitutions.insert_or_assign(name, type);
    }
    TypeRef ret = method.returnType ? ResolveTypeWithSubstitution(*method.returnType->get(), substitutions)
                                    : TypeRef::MakeOpaque();
    currentSelfType = savedSelfType;
    currentTypeParams = savedTypeParams;
    return ret;
}

std::vector<TypeRef>
AnalysisContext::ResolveMethodParamTypes(const TypeRef &receiverType, const FuncDecl &method,
                                         const std::unordered_map<std::string, TypeRef> &methodSubstitutions,
                                         const bool includeReceiver) {
    const auto savedTypeParams = currentTypeParams;
    AppendTypeParameterNames(currentTypeParams, method.typeParams);
    auto substitutions = MethodTypeSubstitutions(receiverType);
    for (const auto &[name, type] : methodSubstitutions) {
        substitutions.insert_or_assign(name, type);
    }
    TypeRef savedSelfType = currentSelfType;
    currentSelfType = receiverType->kind == TypeRef::Kind::Pointer || receiverType->kind == TypeRef::Kind::Reference
                        ? receiverType
                        : TypeRef::MakePointer(receiverType);
    std::vector<TypeRef> params;
    for (const auto &param : method.params) {
        if (param.isVariadic || (!includeReceiver && param.name == "self")) {
            continue;
        }
        params.push_back(ResolveTypeWithSubstitution(*param.type, substitutions));
    }
    currentSelfType = savedSelfType;
    currentTypeParams = savedTypeParams;
    return params;
}

std::optional<TypeRef>
AnalysisContext::ResolveMethodVariadicType(const TypeRef &receiverType, const FuncDecl &method,
                                           const std::unordered_map<std::string, TypeRef> &methodSubstitutions) {
    if (method.params.empty() || !method.params.back().isVariadic) {
        return std::nullopt;
    }
    const auto savedTypeParams = currentTypeParams;
    AppendTypeParameterNames(currentTypeParams, method.typeParams);
    auto substitutions = MethodTypeSubstitutions(receiverType);
    for (const auto &[name, type] : methodSubstitutions) {
        substitutions.insert_or_assign(name, type);
    }
    TypeRef savedSelfType = currentSelfType;
    currentSelfType = receiverType->kind == TypeRef::Kind::Pointer || receiverType->kind == TypeRef::Kind::Reference
                        ? receiverType
                        : TypeRef::MakePointer(receiverType);
    TypeRef element = ResolveTypeWithSubstitution(*method.params.back().type, substitutions);
    currentSelfType = savedSelfType;
    currentTypeParams = savedTypeParams;
    return element;
}

const FuncDecl *AnalysisContext::LookupMethodCall(const TypeRef &receiverType, const std::string &methodName,
                                                  const CallExpr &call, const std::vector<TypeRef> &argumentTypes,
                                                  const std::size_t typeArgumentOffset,
                                                  std::unordered_map<std::string, TypeRef> &substitutions,
                                                  const bool receiverIsArgument) {
    const auto candidates = AccessibleMethodCandidates(receiverType, methodName);
    const std::size_t writtenCount = call.typeArgs.size() - typeArgumentOffset;
    if (writtenCount == 0 &&
        std::ranges::none_of(candidates, [](const FuncDecl *method) { return !method->typeParams.empty(); })) {
        substitutions = MethodTypeSubstitutions(receiverType);
        return LookupMethod(receiverType, methodName, argumentTypes, true, &call, receiverIsArgument);
    }
    for (const FuncDecl *method : candidates) {
        if (writtenCount != 0 && writtenCount != method->typeParams.size()) {
            continue;
        }
        auto resolved = MethodTypeSubstitutions(receiverType);
        std::unordered_set<std::string> parameters;
        for (std::size_t i = 0; i < method->typeParams.size(); ++i) {
            parameters.insert(method->typeParams[i].name);
            if (i < writtenCount) {
                resolved.insert_or_assign(method->typeParams[i].name,
                                          ResolveType(*call.typeArgs[typeArgumentOffset + i]));
            }
        }
        const auto boundTypes = [&] {
            return BoundParameterTypes(*method, !receiverIsArgument,
                                       ResolveMethodParamTypes(receiverType, *method, resolved, receiverIsArgument),
                                       ResolveMethodVariadicType(receiverType, *method, resolved), argumentTypes.size(),
                                       &call);
        };
        auto parameterTypes = boundTypes();
        if (!parameterTypes) {
            continue;
        }
        if (writtenCount == 0) {
            for (std::size_t i = 0; i < argumentTypes.size(); ++i) {
                DeduceTypeArgument((*parameterTypes)[i], argumentTypes[i], parameters, resolved);
            }
        }
        if (std::ranges::any_of(method->typeParams,
                                [&](const TypeParameter &parameter) { return !resolved.contains(parameter.name); })) {
            continue;
        }
        parameterTypes = boundTypes();
        bool matches = parameterTypes.has_value();
        for (std::size_t i = 0; matches && i < argumentTypes.size(); ++i) {
            const auto &argument = argumentTypes[i];
            const auto &parameter = (*parameterTypes)[i];
            if (!argument.IsUnknown() && !parameter.IsUnknown() && !argument.IsAssignableTo(parameter) &&
                !argument.CanImplicitlyBorrowTo(parameter) && !argument.CanReadScalarTo(parameter) &&
                !CanConvertToInterface(argument, parameter) && !(argument.IsInteger() && parameter.IsInteger()) &&
                !DefaultLiteralReachesNative(argument, parameter)) {
                matches = false;
                break;
            }
        }
        if (matches) {
            substitutions = std::move(resolved);
            return method;
        }
    }
    if (!candidates.empty()) {
        EmitError(call.location, std::format("cannot resolve type arguments for method '{}'", methodName));
    }
    return nullptr;
}

const FuncDecl *AnalysisContext::LookupOperatorMethod(const TypeRef &receiverType, const std::string &operatorName,
                                                      const std::vector<TypeRef> &argumentTypes) {
    return LookupMethod(receiverType, operatorName, argumentTypes);
}

std::vector<TypeRef> AnalysisContext::ResolveOperatorParameterTypes(const TypeRef &receiverType,
                                                                    const FuncDecl &method) {
    return ResolveMethodParamTypes(receiverType, method);
}

TypeRef AnalysisContext::ResolveOperatorReturnType(const TypeRef &receiverType, const FuncDecl &method) {
    return ResolveMethodReturnType(receiverType, method);
}

TypeRef AnalysisContext::AssociatedFunctionType(const TypeRef &receiverType, const FuncDecl &method) {
    TypeRef savedSelfType = currentSelfType;
    currentSelfType = receiverType->kind == TypeRef::Kind::Pointer || receiverType->kind == TypeRef::Kind::Reference
                        ? receiverType
                        : TypeRef::MakePointer(receiverType);
    TypeRef type = MakeFuncTypeWithSubstitution(method.params, method.returnType, MethodTypeSubstitutions(receiverType),
                                                TypeParameterNames(method.typeParams));
    currentSelfType = savedSelfType;
    return type;
}

[[nodiscard]] const FuncDecl *AnalysisContext::LookupInterfaceMethod(const TypeRef &receiverType,
                                                                     const std::string &methodName) const {
    const std::string ifaceName = NamedBaseTypeName(receiverType);
    if (ifaceName.empty()) {
        return nullptr;
    }
    const auto ifaceIt = interfaceDecls.find(ifaceName);
    if (ifaceIt == interfaceDecls.end()) {
        return nullptr;
    }
    for (const auto &method : ifaceIt->second->methods) {
        if (method->name == methodName) {
            return method.get();
        }
    }
    return nullptr;
}

/// `Self` bound to the type the interface is being read for.
std::unordered_map<std::string, TypeRef> AnalysisContext::SelfSubstitution(const TypeRef &selfType) {
    return {{std::string(SemanticDetail::SelfTypeName), selfType}};
}

TypeRef AnalysisContext::ResolveRequirementType(const TypeExpr &type, const TypeRef &selfType,
                                                const InterfaceDecl *interface,
                                                const std::vector<TypeRef> &interfaceArguments) {
    auto substitution = SelfSubstitution(selfType);
    if (interface == nullptr || interface->typeParams.empty()) {
        return ResolveTypeWithSubstitution(type, substitution);
    }
    // The requirement is written in terms of the interface's own parameters, so they are named as type parameters
    // while it is read, whatever generic happens to be reading it.
    const auto savedTypeParams = currentTypeParams;
    AppendTypeParameterNames(currentTypeParams, interface->typeParams);
    for (std::size_t index = 0; index < interface->typeParams.size() && index < interfaceArguments.size(); ++index) {
        substitution.insert_or_assign(interface->typeParams[index].name, interfaceArguments[index]);
    }
    TypeRef resolved = ResolveTypeWithSubstitution(type, substitution);
    currentTypeParams = savedTypeParams;
    return resolved;
}

TypeRef AnalysisContext::ResolveInterfaceMethodReturnType(const FuncDecl &method, const TypeRef &selfType,
                                                          const InterfaceDecl *interface,
                                                          const std::vector<TypeRef> &interfaceArguments) {
    if (!method.returnType) {
        return TypeRef::MakeOpaque();
    }
    return ResolveRequirementType(*method.returnType->get(), selfType, interface, interfaceArguments);
}

std::optional<TypeRef>
AnalysisContext::ResolveInterfaceMethodVariadicType(const FuncDecl &method, const TypeRef &selfType,
                                                    const InterfaceDecl *interface,
                                                    const std::vector<TypeRef> &interfaceArguments) {
    if (method.params.empty() || !method.params.back().isVariadic) {
        return std::nullopt;
    }
    return ResolveRequirementType(*method.params.back().type, selfType, interface, interfaceArguments);
}

std::optional<AnalysisContext::SignatureMismatch>
AnalysisContext::RequirementSignatureMismatch(const TypeRef &receiver, const FuncDecl &method,
                                              const FuncDecl &requirement, const InterfaceDecl &interface,
                                              const std::vector<TypeRef> &interfaceArguments) {
    const std::vector<TypeRef> requiredParameters =
        ResolveInterfaceMethodParamTypes(requirement, receiver, &interface, interfaceArguments);
    const std::optional<TypeRef> requiredVariadic =
        ResolveInterfaceMethodVariadicType(requirement, receiver, &interface, interfaceArguments);
    const TypeRef requiredResult =
        ResolveInterfaceMethodReturnType(requirement, receiver, &interface, interfaceArguments);
    const std::vector<TypeRef> declaredParameters = ResolveMethodParamTypes(receiver, method, {}, false);
    const std::optional<TypeRef> declaredVariadic = ResolveMethodVariadicType(receiver, method);
    const TypeRef declaredResult = ResolveMethodReturnType(receiver, method);

    const auto unknown = [](const TypeRef &type) { return type.IsUnknown() || type->kind == TypeRef::Kind::TypeParam; };
    if (std::ranges::any_of(requiredParameters, unknown) || std::ranges::any_of(declaredParameters, unknown) ||
        unknown(requiredResult) || unknown(declaredResult) || (requiredVariadic && unknown(*requiredVariadic)) ||
        (declaredVariadic && unknown(*declaredVariadic))) {
        return std::nullopt;
    }
    if (requiredParameters == declaredParameters && requiredResult == declaredResult &&
        requiredVariadic.has_value() == declaredVariadic.has_value() &&
        (!requiredVariadic || *requiredVariadic == *declaredVariadic)) {
        return std::nullopt;
    }
    const auto spell = [](const std::vector<TypeRef> &parameters, const std::optional<TypeRef> &variadic,
                          const TypeRef &result) {
        std::string text = "(";
        for (std::size_t index = 0; index < parameters.size(); ++index) {
            text += (index == 0 ? "" : ", ") + parameters[index].DisplayString();
        }
        if (variadic) {
            text += (parameters.empty() ? "" : ", ") + variadic->DisplayString() + "...";
        }
        text += ")";
        if (!result.IsOpaque()) {
            text += " -> " + result.DisplayString();
        }
        return text;
    };
    return SignatureMismatch{spell(requiredParameters, requiredVariadic, requiredResult),
                             spell(declaredParameters, declaredVariadic, declaredResult)};
}

std::vector<TypeRef> AnalysisContext::ResolveInterfaceMethodParamTypes(const FuncDecl &method, const TypeRef &selfType,
                                                                       const InterfaceDecl *interface,
                                                                       const std::vector<TypeRef> &interfaceArguments) {
    std::vector<TypeRef> params;
    for (const auto &param : method.params) {
        // The receiver arrives as the data half of the interface value, so it is not one of the written arguments.
        if (param.isVariadic || param.IsReceiver()) {
            continue;
        }
        params.push_back(ResolveRequirementType(*param.type, selfType, interface, interfaceArguments));
    }
    return params;
}

const FuncDecl *AnalysisContext::LookupFunctionOverload(const Symbol &sym, const std::vector<TypeRef> &argTypes,
                                                        const std::vector<TypeExprPtr> &typeArgs,
                                                        const CallExpr *call) {
    ambiguousOverloads.clear();
    if (sym.kind != Symbol::Kind::Func || sym.funcOverloads.empty()) {
        return nullptr;
    }
    if (sym.funcOverloads.size() == 1) {
        const auto *decl = sym.funcOverloads[0];
        if (!typeArgs.empty() && typeArgs.size() != decl->typeParams.size()) {
            return decl;
        }
        std::unordered_map<std::string, TypeRef> substitutions;
        const std::size_t count = std::min(decl->typeParams.size(), typeArgs.size());
        for (std::size_t i = 0; i < count; ++i) {
            substitutions.emplace(decl->typeParams[i].name, ResolveType(*typeArgs[i]));
        }
        if (substitutions.size() < decl->typeParams.size()) {
            DeduceTypeArguments(*decl, argTypes, substitutions, call);
        }
        TypeRef funcType = MakeFuncTypeWithSubstitution(decl->params, decl->returnType, substitutions,
                                                        TypeParameterNames(decl->typeParams));
        if (funcType->kind != TypeRef::Kind::Func || funcType->inner.empty()) {
            return decl;
        }
        const std::size_t paramCount = funcType->inner.size() - 1;
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
            return nullptr;
        }
        for (std::size_t i = 0; i < std::min(argTypes.size(), paramCount); ++i) {
            if (argTypes[i].IsUnknown() || funcType->inner[i].IsUnknown()) {
                continue;
            }
            // A literal array or tuple is typed element by element, so `[3, 4]` reaches an `int32[2]` parameter.
            const bool literalAggregate =
                call != nullptr && call->args.size() == argTypes.size() &&
                (AstCast<const ArrayExpr>(call->args[i].get()) || AstCast<const TupleExpr>(call->args[i].get())) &&
                CanAssignExprTo(*call->args[i], argTypes[i], funcType->inner[i]);
            if (!argTypes[i].IsAssignableTo(funcType->inner[i]) && !argTypes[i].CanReadScalarTo(funcType->inner[i]) &&
                !argTypes[i].CanImplicitlyBorrowTo(funcType->inner[i]) &&
                !CanConvertToInterface(argTypes[i], funcType->inner[i]) &&
                !(argTypes[i].IsInteger() && funcType->inner[i].IsInteger()) &&
                !DefaultLiteralReachesNative(argTypes[i], funcType->inner[i]) && !literalAggregate) {
                return nullptr;
            }
        }
        return decl;
    }
    // Every overload that accepts the call is collected in one pass and ranked by how well the arguments convert, so
    // neither declaration order nor arity decides between overloads that convert differently.
    std::vector<OverloadMatch> matches;
    for (const auto *decl : sym.funcOverloads) {
        if (!typeArgs.empty() && typeArgs.size() != decl->typeParams.size()) {
            continue;
        }
        std::unordered_map<std::string, TypeRef> substitutions;
        const std::size_t count = std::min(decl->typeParams.size(), typeArgs.size());
        for (std::size_t i = 0; i < count; ++i) {
            substitutions.emplace(decl->typeParams[i].name, ResolveType(*typeArgs[i]));
        }
        if (substitutions.size() < decl->typeParams.size()) {
            DeduceTypeArguments(*decl, argTypes, substitutions, call);
        }
        if (!TypeArgumentsSatisfyBounds(decl->typeParams, substitutions)) {
            continue;
        }
        TypeRef funcType = MakeFuncTypeWithSubstitution(decl->params, decl->returnType, substitutions,
                                                        TypeParameterNames(decl->typeParams));
        if (funcType->kind != TypeRef::Kind::Func || funcType->inner.empty()) {
            continue;
        }
        const std::vector<TypeRef> fixedTypes(funcType->inner.begin(), funcType->inner.end() - 1);
        const auto boundTypes = BoundParameterTypes(
            *decl, false, fixedTypes, ResolveVariadicElementType(*decl, substitutions), argTypes.size(), call);
        if (!boundTypes) {
            continue;
        }
        if (ArgumentsReachOverload(argTypes, *boundTypes, call)) {
            matches.push_back({decl, *boundTypes});
        }
    }
    return SelectOverload(matches, argTypes);
}

std::string AnalysisContext::AmbiguousCallHelp(const std::string_view authorHelp) const {
    const bool callerOwnsOne = std::ranges::any_of(ambiguousOverloads, [this](const FuncDecl *overload) {
        const auto found = declarationInfos.find(overload);
        return found == declarationInfos.end() || found->second.ownerPackage == currentPackage;
    });
    if (callerOwnsOne) {
        return std::string(authorHelp);
    }
    return "give an unsuffixed literal argument the width it means, as in '3i64', or convert an argument with 'as'";
}

bool AnalysisContext::ArgumentsReachOverload(const std::vector<TypeRef> &argTypes,
                                             const std::vector<TypeRef> &parameterTypes, const CallExpr *call) {
    // The written arguments, when the call is known, so a literal is told apart from a value that has its type.
    const bool expressions = call != nullptr && call->args.size() == argTypes.size();
    for (std::size_t i = 0; i < argTypes.size() && i < parameterTypes.size(); ++i) {
        const TypeRef &paramType = parameterTypes[i];
        if (argTypes[i].IsUnknown() || paramType.IsUnknown()) {
            continue;
        }
        // An unsuffixed integer literal carries the default `int`, which is assignable to no other integer width, so
        // it is let reach any integer parameter it fits; a variable of type `int`, like any explicitly typed argument,
        // is never silently narrowed to another width. Without the call, every `int` argument is taken as a literal.
        const bool literalToInteger = (argTypes[i]->kind == TypeRef::Kind::Int && paramType.IsInteger() &&
                                       (!expressions || CanAssignExprTo(*call->args[i], argTypes[i], paramType))) ||
                                      DefaultLiteralReachesNative(argTypes[i], paramType);
        // A literal array or tuple is typed element by element, so `[3, 4]` reaches an `int32[2]` parameter.
        const bool literalAggregate =
            expressions &&
            (AstCast<const ArrayExpr>(call->args[i].get()) || AstCast<const TupleExpr>(call->args[i].get())) &&
            CanAssignExprTo(*call->args[i], argTypes[i], paramType);
        const bool assignable = argTypes[i].IsAssignableTo(paramType) || argTypes[i].CanReadScalarTo(paramType) ||
                                argTypes[i].CanImplicitlyBorrowTo(paramType) ||
                                CanConvertToInterface(argTypes[i], paramType) || literalToInteger || literalAggregate;
        if (!assignable) {
            return false;
        }
    }
    return true;
}

const FuncDecl *AnalysisContext::SelectOverload(const std::vector<OverloadMatch> &matches,
                                                const std::vector<TypeRef> &argumentTypes) {
    if (matches.empty()) {
        return nullptr;
    }
    // How far an argument is from a parameter: the same type; the same type differing only in a view's writability;
    // the same type reached through a borrow or a scalar read; or another type it converts to. An overload no worse on
    // any argument and better on one is more specific, so an `&float32` argument selects the `float32` overload over
    // the `float64` one it would also widen to, and an `&var int32` the `&int32` overload over the `int32` one.
    const auto cost = [](const TypeRef &argument, const TypeRef &parameter) {
        if (argument == parameter) {
            return 0;
        }
        const auto readOnly = [](TypeRef type) {
            type.isMut = false;
            if (!type->inner.empty() &&
                (type->kind == TypeRef::Kind::Reference || type->kind == TypeRef::Kind::Pointer || type.IsSlice())) {
                type = type.WithWritableInner(false);
            }
            return type;
        };
        if (readOnly(argument) == readOnly(parameter)) {
            return 1;
        }
        const auto referent = [&](const TypeRef &type) {
            return type->kind == TypeRef::Kind::Reference && !type->inner.empty() ? readOnly(type->inner.front())
                                                                                  : readOnly(type);
        };
        return referent(argument) == referent(parameter) ? 2 : 3;
    };
    const auto noWorse = [&](const OverloadMatch &left, const OverloadMatch &right, bool &better) {
        better = false;
        const std::size_t count =
            std::min({argumentTypes.size(), left.parameterTypes.size(), right.parameterTypes.size()});
        for (std::size_t index = 0; index < count; ++index) {
            const int leftCost = cost(argumentTypes[index], left.parameterTypes[index]);
            const int rightCost = cost(argumentTypes[index], right.parameterTypes[index]);
            if (leftCost > rightCost) {
                return false;
            }
            better = better || leftCost < rightCost;
        }
        return true;
    };
    std::vector<const FuncDecl *> mostSpecific;
    for (const OverloadMatch &candidate : matches) {
        // One declaration reached through two imports is one candidate.
        if (std::ranges::contains(mostSpecific, candidate.decl)) {
            continue;
        }
        const bool dominated = std::ranges::any_of(matches, [&](const OverloadMatch &other) {
            bool better = false;
            return &other != &candidate && noWorse(other, candidate, better) && better;
        });
        if (!dominated) {
            mostSpecific.push_back(candidate.decl);
        }
    }
    const std::size_t argumentCount = argumentTypes.size();
    const std::vector<const FuncDecl *> &tied = mostSpecific;
    if (tied.size() == 1) {
        return tied.front();
    }
    // Among overloads that accept the call equally well, the one the arguments fill exactly — every fixed parameter
    // given, no default value, no variadic tail — wins, so `D(2)` calls `D(x)` rather than `D(x, y = 1)` or
    // `D(x, rest...)`; then a function that is not generic wins over one that is.
    const auto fillsExactly = [&](const FuncDecl *decl) {
        std::size_t fixed = 0;
        for (const Param &parameter : decl->params) {
            if (parameter.isVariadic) {
                return false;
            }
            if (!parameter.IsReceiver()) {
                ++fixed;
            }
        }
        return fixed == argumentCount;
    };
    std::vector<const FuncDecl *> preferred;
    for (const FuncDecl *decl : tied) {
        if (fillsExactly(decl)) {
            preferred.push_back(decl);
        }
    }
    if (preferred.empty()) {
        preferred = tied;
    }
    if (preferred.size() > 1) {
        std::vector<const FuncDecl *> concrete;
        for (const FuncDecl *decl : preferred) {
            if (decl->typeParams.empty()) {
                concrete.push_back(decl);
            }
        }
        if (!concrete.empty()) {
            preferred = std::move(concrete);
        }
    }
    if (preferred.size() == 1) {
        return preferred.front();
    }
    ambiguousOverloads = preferred;
    return nullptr;
}

bool AnalysisContext::DefaultLiteralReachesNative(const TypeRef &argument, const TypeRef &parameter) {
    // `none` or a constructor whose other channel is still open takes its type from the parameter, and so does an
    // array of them; the argument's own conversion is checked against the selected parameter.
    if (argument.MentionsIncompleteNative()) {
        return true;
    }
    // A tuple literal reaches a tuple parameter element by element, so `Where((3, 0))` finds `Where(p: (int32,
    // int32))` although its elements carry the default `int`; whether each element fits is the argument's own check.
    if (argument->kind == TypeRef::Kind::Tuple && parameter->kind == TypeRef::Kind::Tuple &&
        argument->inner.size() == parameter->inner.size()) {
        for (std::size_t index = 0; index < argument->inner.size(); ++index) {
            const TypeRef &element = argument->inner[index];
            const TypeRef &expected = parameter->inner[index];
            const bool reaches = element.IsAssignableTo(expected) ||
                                 (element->kind == TypeRef::Kind::Int && expected.IsInteger()) ||
                                 (element->kind == TypeRef::Kind::Float64 && expected.IsFloat()) ||
                                 DefaultLiteralReachesNative(element, expected);
            if (!reaches) {
                return false;
            }
        }
        return true;
    }
    // An unsuffixed literal carries its default width until a destination gives it one. A native parameter does so the
    // way the conversion rules do: through presence and success levels, then to the one member of its literal kind.
    // The argument's own check still rejects a value that does not fit, or an argument that is no literal at all.
    const bool integer = argument->kind == TypeRef::Kind::Int;
    const bool floating = argument->kind == TypeRef::Kind::Float64;
    if ((!integer && !floating) || !(parameter.IsSum() || parameter.IsOptional() || parameter.IsFallible())) {
        return false;
    }
    TypeRef level = parameter;
    while (level.IsOptional() || level.IsFallible()) {
        TypeRef payload = level->inner.front();
        level = payload;
    }
    if (!level.IsSum()) {
        return integer ? level.IsInteger() : level.IsFloat();
    }
    return std::ranges::count_if(level->inner, [&](const TypeRef &member) {
               return integer ? member.IsInteger() : member.IsFloat();
           }) == 1;
}

TypeRef AnalysisContext::FunctionType(const FuncDecl &decl) {
    return MakeFuncType(decl.params, decl.returnType, TypeParameterNames(decl.typeParams));
}
} // namespace Rux::SemanticDetail
