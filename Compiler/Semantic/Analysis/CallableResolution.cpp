#include "Lexer/Lexer.h"
#include "Numeric/IntegerLiteral.h"
#include "Semantic/Analysis/AnalysisContext.h"
#include "Semantic/Conditional/ConditionalCompilation.h"
#include "Semantic/Model/PrimitiveConstants.h"
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
    if ((receiver->kind == TypeRef::Kind::Pointer || receiver->kind == TypeRef::Kind::Reference) &&
        !receiver->inner.empty()) {
        receiver = &receiver->inner[0];
    }
    if (receiver->kind != TypeRef::Kind::Named) {
        return {};
    }

    const std::vector<TypeParameter> *typeParams = AggregateTypeParams(BaseTypeName(receiver->name));
    if (!typeParams) {
        return {};
    }
    const std::vector<TypeRef> args = ParseTypeArgsFromTypeName(receiver->name);
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
    for (std::size_t i = 0; i < typeArgs.size(); ++i) {
        if (i) {
            name += ", ";
        }
        name += ResolveType(*typeArgs[i]).ToString();
    }
    name += ">";
    return TypeRef::MakeNamed(std::move(name));
}

TypeRef AnalysisContext::ResolveMethodReturnType(const TypeRef &receiverType, const FuncDecl &method) {
    TypeRef savedSelfType = currentSelfType;
    currentSelfType = receiverType.kind == TypeRef::Kind::Pointer || receiverType.kind == TypeRef::Kind::Reference
                        ? receiverType
                        : TypeRef::MakePointer(receiverType);
    const auto substitutions = MethodTypeSubstitutions(receiverType);
    TypeRef ret = method.returnType ? ResolveTypeWithSubstitution(*method.returnType->get(), substitutions)
                                    : TypeRef::MakeOpaque();
    currentSelfType = savedSelfType;
    return ret;
}

std::vector<TypeRef> AnalysisContext::ResolveMethodParamTypes(const TypeRef &receiverType, const FuncDecl &method) {
    TypeRef savedSelfType = currentSelfType;
    currentSelfType = receiverType.kind == TypeRef::Kind::Pointer || receiverType.kind == TypeRef::Kind::Reference
                        ? receiverType
                        : TypeRef::MakePointer(receiverType);
    std::vector<TypeRef> params;
    for (const auto &param : method.params) {
        if (param.isVariadic || param.name == "self") {
            continue;
        }
        params.push_back(ResolveTypeWithSubstitution(*param.type, MethodTypeSubstitutions(receiverType)));
    }
    currentSelfType = savedSelfType;
    return params;
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
    currentSelfType = receiverType.kind == TypeRef::Kind::Pointer || receiverType.kind == TypeRef::Kind::Reference
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

TypeRef AnalysisContext::ResolveInterfaceMethodReturnType(const FuncDecl &method, const TypeRef &selfType) {
    if (!method.returnType) {
        return TypeRef::MakeOpaque();
    }
    return ResolveTypeWithSubstitution(*method.returnType->get(), SelfSubstitution(selfType));
}

std::vector<TypeRef> AnalysisContext::ResolveInterfaceMethodParamTypes(const FuncDecl &method,
                                                                       const TypeRef &selfType) {
    const auto substitution = SelfSubstitution(selfType);
    std::vector<TypeRef> params;
    for (const auto &param : method.params) {
        // The receiver arrives as the data half of the interface value, so it is not one of the written arguments.
        if (param.isVariadic || param.IsReceiver()) {
            continue;
        }
        params.push_back(ResolveTypeWithSubstitution(*param.type, substitution));
    }
    return params;
}

const FuncDecl *AnalysisContext::LookupFunctionOverload(const Symbol &sym, const std::vector<TypeRef> &argTypes,
                                                        const std::vector<TypeExprPtr> &typeArgs) {
    if (sym.kind != Symbol::Kind::Func || sym.funcOverloads.empty()) {
        return nullptr;
    }
    const auto borrowsAsInterface = [&](const TypeRef &argument, const TypeRef &parameter) {
        if (parameter.kind != TypeRef::Kind::Reference || parameter.inner.empty()) {
            return false;
        }
        TypeRef source =
            argument.kind == TypeRef::Kind::Reference && !argument.inner.empty() ? argument.inner.front() : argument;
        TypeRef target = parameter.inner.front();
        source.isMut = false;
        target.isMut = false;
        return TypeImplementsInterface(source, target);
    };
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
            DeduceTypeArguments(*decl, argTypes, substitutions);
        }
        TypeRef funcType = MakeFuncTypeWithSubstitution(decl->params, decl->returnType, substitutions,
                                                        TypeParameterNames(decl->typeParams));
        if (funcType.kind != TypeRef::Kind::Func || funcType.inner.empty()) {
            return decl;
        }
        const std::size_t paramCount = funcType.inner.size() - 1;
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
            if (argTypes[i].IsUnknown() || funcType.inner[i].IsUnknown()) {
                continue;
            }
            if (!argTypes[i].IsAssignableTo(funcType.inner[i]) &&
                !argTypes[i].CanImplicitlyBorrowTo(funcType.inner[i]) &&
                !borrowsAsInterface(argTypes[i], funcType.inner[i]) &&
                !(argTypes[i].IsInteger() && funcType.inner[i].IsInteger())) {
                return nullptr;
            }
        }
        return decl;
    }
    for (const bool allowVariadic : {false, true}) {
        for (const bool exactOnly : {true, false}) {
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
                    DeduceTypeArguments(*decl, argTypes, substitutions);
                }
                if (!TypeArgumentsSatisfyBounds(decl->typeParams, substitutions)) {
                    continue;
                }
                TypeRef funcType = MakeFuncTypeWithSubstitution(decl->params, decl->returnType, substitutions,
                                                                TypeParameterNames(decl->typeParams));
                if (funcType.kind != TypeRef::Kind::Func || funcType.inner.empty()) {
                    continue;
                }
                const std::size_t paramCount = funcType.inner.size() - 1;
                const bool isVariadic = !decl->params.empty() && decl->params.back().isVariadic;
                if (isVariadic != allowVariadic) {
                    continue;
                }
                std::size_t requiredCount = 0;
                for (const auto &p : decl->params) {
                    if (!p.isVariadic && !p.defaultValue) {
                        ++requiredCount;
                    }
                }
                const bool arityOk = isVariadic ? argTypes.size() >= requiredCount
                                                : (argTypes.size() >= requiredCount && argTypes.size() <= paramCount);
                if (!arityOk) {
                    continue;
                }
                bool match = true;
                for (std::size_t i = 0; i < std::min(argTypes.size(), paramCount); ++i) {
                    const TypeRef &paramType = funcType.inner[i];
                    if (argTypes[i].IsUnknown() || paramType.IsUnknown()) {
                        continue;
                    }
                    // An unsuffixed integer literal carries the default `int`, which is assignable to no other
                    // integer width. The single-overload path above lets one reach any integer parameter anyway;
                    // without the same allowance here, giving a name a second overload stopped every call that
                    // passed a bare literal from resolving at all. Only the coercing pass grants it, so an exact
                    // match still wins, and only `int` widens, so an explicitly typed argument is never silently
                    // narrowed to a different width.
                    const bool literalToInteger = argTypes[i].kind == TypeRef::Kind::Int && paramType.IsInteger();
                    const bool assignable = argTypes[i].IsAssignableTo(paramType) ||
                                            argTypes[i].CanImplicitlyBorrowTo(paramType) ||
                                            borrowsAsInterface(argTypes[i], paramType) || literalToInteger;
                    if (exactOnly ? !(argTypes[i] == paramType) : !assignable) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    return decl;
                }
            }
        }
    }
    return nullptr;
}

TypeRef AnalysisContext::FunctionType(const FuncDecl &decl) {
    return MakeFuncType(decl.params, decl.returnType, TypeParameterNames(decl.typeParams));
}
} // namespace Rux::SemanticDetail
