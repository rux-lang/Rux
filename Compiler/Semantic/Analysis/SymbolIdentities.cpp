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

TypeRef AnalysisContext::SubstituteIdentityType(TypeRef type,
                                                const std::unordered_map<std::string, TypeRef> &substitutions) const {
    if (type.kind == TypeRef::Kind::TypeParam) {
        if (const auto substitution = substitutions.find(type.name); substitution != substitutions.end()) {
            return substitution->second;
        }
    }
    for (auto &inner : type.inner) {
        inner = SubstituteIdentityType(std::move(inner), substitutions);
    }
    return type;
}

TypeRef AnalysisContext::IdentityParameterType(const Param &parameter,
                                               const std::unordered_map<std::string, TypeRef> &substitutions) const {
    TypeRef type = TypeRef::MakeUnknown();
    if (const auto resolved = typeNodeTypes.find(parameter.type.get()); resolved != typeNodeTypes.end()) {
        type = SubstituteIdentityType(resolved->second, substitutions);
    }
    if (parameter.isVariadic) {
        type = TypeRef::MakeSlice(type);
    }
    return type;
}

std::string AnalysisContext::MangleFunctionWithParams(const FuncDecl &declaration) const {
    std::string name = declaration.name + "__";
    for (std::size_t i = 0; i < declaration.params.size(); ++i) {
        if (i != 0) {
            name += "_";
        }
        name += MangleTypeName(IdentityParameterType(declaration.params[i]));
    }
    return name;
}

bool AnalysisContext::FunctionIsOverloadedInModule(const FuncDecl &declaration) const {
    const auto functions = functionsByName.find(declaration.name);
    if (functions == functionsByName.end()) {
        return false;
    }
    const std::string &modulePath = functionModulePaths.at(&declaration);
    std::size_t count = 0;
    for (const auto *candidate : functions->second) {
        if (functionModulePaths.at(candidate) == modulePath && ++count > 1) {
            return true;
        }
    }
    return false;
}

bool AnalysisContext::MethodIsOverloadedForIdentity(const std::string &typeName, const std::string &methodName) const {
    const auto type = methodsByType.find(typeName);
    if (type == methodsByType.end()) {
        return false;
    }
    const auto method = type->second.find(methodName);
    return method != type->second.end() && method->second.size() > 1;
}

std::string AnalysisContext::MethodLinkerName(const FuncDecl &method, const TypeRef &receiverType,
                                              const std::unordered_map<std::string, TypeRef> &substitutions) const {
    const std::string typeName = NamedBaseTypeName(receiverType);
    std::string name = typeName + "::" + method.name;
    if (MethodIsOverloadedForIdentity(typeName, method.name)) {
        name += "__";
        bool first = true;
        for (const auto &parameter : method.params) {
            if (parameter.name == "self" || parameter.isVariadic) {
                continue;
            }
            if (!first) {
                name += "_";
            }
            name += MangleTypeName(IdentityParameterType(parameter, substitutions));
            first = false;
        }
    }

    const auto implementation = methodImpls.find(&method);
    if (implementation == methodImpls.end() || ImplTypeParams(*implementation->second).empty()) {
        return name;
    }
    if (const std::vector<TypeParameter> *typeParams = AggregateTypeParams(typeName)) {
        for (const auto &parameter : *typeParams) {
            if (const auto substitution = substitutions.find(parameter.name); substitution != substitutions.end()) {
                name += "_" + MangleTypeName(substitution->second);
            }
        }
    }
    return name;
}

void AnalysisContext::RecordMethodIdentityRecipe(ResolvedCallableBinding &binding, const FuncDecl &method) const {
    assert(binding.receiverType && "method binding is missing its receiver type");
    const std::string typeName = NamedBaseTypeName(*binding.receiverType);
    binding.linkerNameBase = typeName + "::" + method.name;
    binding.linkerNameHasOverloadSignature = MethodIsOverloadedForIdentity(typeName, method.name);
    if (binding.linkerNameHasOverloadSignature) {
        for (const auto &parameter : method.params) {
            if (parameter.name != "self" && !parameter.isVariadic) {
                binding.linkerOverloadTypes.push_back(IdentityParameterType(parameter, binding.substitutions));
            }
        }
    }

    const auto implementation = methodImpls.find(&method);
    if (implementation == methodImpls.end() || ImplTypeParams(*implementation->second).empty()) {
        return;
    }
    if (const std::vector<TypeParameter> *typeParams = AggregateTypeParams(typeName)) {
        binding.linkerSpecializationParameters = TypeParameterNames(*typeParams);
    }
}

void AnalysisContext::BuildFinalSymbolIdentities() {
    std::unordered_map<std::string, std::unordered_set<std::string>> owners;
    for (const auto &[name, declarations] : functionsByName) {
        (void)name;
        for (const auto *declaration : declarations) {
            const std::string local =
                FunctionIsOverloadedInModule(*declaration) ? MangleFunctionWithParams(*declaration) : declaration->name;
            owners[local].insert(functionModulePaths.at(declaration));
        }
    }

    const auto localFunctionName = [&](const FuncDecl &declaration) {
        return FunctionIsOverloadedInModule(declaration) ? MangleFunctionWithParams(declaration) : declaration.name;
    };
    const auto qualifyFunctionName = [&](const FuncDecl &declaration, std::string name) {
        const std::string local = localFunctionName(declaration);
        const std::string &modulePath = functionModulePaths.at(&declaration);
        if (owners[local].size() > 1 && !modulePath.empty()) {
            return modulePath + "::" + name;
        }
        return name;
    };

    for (const auto &[name, declarations] : functionsByName) {
        (void)name;
        for (const auto *declaration : declarations) {
            std::string local = qualifyFunctionName(*declaration, localFunctionName(*declaration));
            symbolIdentities.insert_or_assign(declaration, ResolvedSymbolIdentity{std::move(local)});
        }
    }

    for (const auto *declaration : externFuncDecls) {
        symbolIdentities.insert_or_assign(
            declaration,
            ResolvedSymbolIdentity{declaration->symbolName.empty() ? declaration->name : declaration->symbolName});
    }

    for (const auto *implementation : implDecls) {
        const std::string typeName = implementation->typeName.starts_with("Slice<")
                                       ? implementation->typeName
                                       : BaseTypeName(implementation->typeName);
        if (ImplTypeParams(*implementation).empty()) {
            const auto resolved = typeNodeTypes.find(implementation->extendedType.get());
            const TypeRef receiverType =
                resolved == typeNodeTypes.end() ? TypeRef::MakeNamed(typeName) : resolved->second;
            for (const auto &method : implementation->methods) {
                symbolIdentities.insert_or_assign(method.get(),
                                                  ResolvedSymbolIdentity{MethodLinkerName(*method, receiverType, {})});
            }
        }
        if (!implementation->interfaceName || !ImplTypeParams(*implementation).empty()) {
            continue;
        }
        const auto interface = interfaceDecls.find(*implementation->interfaceName);
        if (interface == interfaceDecls.end() || interface->second->methods.empty()) {
            continue;
        }
        ResolvedVtableIdentity identity;
        identity.linkerName = "__vtable__" + typeName + "__" + *implementation->interfaceName;
        for (const auto &method : interface->second->methods) {
            identity.entries.push_back(typeName + "::" + method->name);
        }
        vtableIdentities.insert_or_assign(implementation, std::move(identity));
    }

    for (auto &[call, binding] : callableBindings) {
        (void)call;
        if (!binding.selectedDeclaration || binding.dispatch == ResolvedCallableBinding::DispatchKind::Interface ||
            binding.dispatch == ResolvedCallableBinding::DispatchKind::Indirect ||
            binding.dispatch == ResolvedCallableBinding::DispatchKind::EnumVariant ||
            // A constrained call has one target per instantiation; lowering names it from the witness instead.
            binding.dispatch == ResolvedCallableBinding::DispatchKind::Constrained) {
            continue;
        }
        const auto *function = dynamic_cast<const FuncDecl *>(binding.selectedDeclaration);
        if (function &&
            (binding.dispatch == ResolvedCallableBinding::DispatchKind::Method ||
             binding.dispatch == ResolvedCallableBinding::DispatchKind::Constructor) &&
            binding.receiverType) {
            binding.linkerName = MethodLinkerName(*function, *binding.receiverType, binding.substitutions);
            RecordMethodIdentityRecipe(binding, *function);
            continue;
        }
        if (function && !function->typeParams.empty()) {
            // A generic function's monomorphized name is its own name plus its type arguments, which two
            // overloads instantiated at the same argument share. Only the first was ever emitted, and every call
            // bound to it — so a four-argument call reached a three-parameter function and quietly dropped an
            // argument. Overloaded methods already carry their parameter types in the name; free functions now
            // do too, built through the same recipe so a later re-instantiation spells it identically.
            // The source declaration and every concrete instantiation must share the same package-qualified base.
            // Otherwise two dependencies exporting the same generic signature both publish (for example)
            // `Clamp__T_T_T`, and calls would still name an unqualified `Clamp_int_int_int` after fixing only the
            // declaration identity.
            binding.linkerNameBase = qualifyFunctionName(*function, function->name);
            binding.linkerNameHasOverloadSignature = FunctionIsOverloadedInModule(*function);
            if (binding.linkerNameHasOverloadSignature) {
                for (const auto &parameter : function->params) {
                    if (!parameter.isVariadic) {
                        binding.linkerOverloadTypes.push_back(IdentityParameterType(parameter, binding.substitutions));
                    }
                }
            }
            binding.linkerSpecializationParameters = TypeParameterNames(function->typeParams);
            binding.linkerName = binding.LinkerNameFor(binding.substitutions);
            continue;
        }
        if (const auto identity = symbolIdentities.find(binding.selectedDeclaration);
            identity != symbolIdentities.end()) {
            binding.linkerName = identity->second.linkerName;
        }
    }
}

// Expressions
TypeRef AnalysisContext::CheckExpr(const Expr &expr) {
    const std::size_t diagnosticStart = diags.size();
    TypeRef type = CheckExprImpl(expr);
    if (const auto *call = dynamic_cast<const CallExpr *>(&expr)) {
        const bool hasNewError =
            std::ranges::any_of(diags.begin() + static_cast<std::ptrdiff_t>(diagnosticStart), diags.end(),
                                [](const SemanticDiagnostic &diagnostic) {
                                    return diagnostic.severity == SemanticDiagnostic::Severity::Error;
                                });
        if (type.IsUnknown() || hasNewError) {
            callableBindings.erase(call);
        }
    }
    RecordCheckedExpression(expr, type);
    return type;
}
} // namespace Rux::SemanticDetail
