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
    if (type->kind == TypeRef::Kind::TypeParam) {
        if (const auto substitution = substitutions.find(type->name); substitution != substitutions.end()) {
            TypeRef substituted = substitution->second;
            // Preserve the same pointee/referent identity used by layout substitution and callable instantiation.
            substituted.isMut = type.isMut;
            return substituted;
        }
    }
    std::vector<TypeRef> inner;
    inner.reserve(type->inner.size());
    for (const TypeRef &member : type->inner) {
        inner.push_back(SubstituteIdentityType(member, substitutions));
    }
    return TypeRef::Renormalize(type.WithInner(inner));
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

std::optional<TypeRef> AnalysisContext::SpecializedExtensionType(const FuncDecl &method) const {
    const auto implementation = methodImpls.find(&method);
    if (implementation == methodImpls.end() || !ImplTypeParams(*implementation->second).empty() ||
        !implementation->second->extendedType) {
        return std::nullopt;
    }
    const auto extended = typeNodeTypes.find(implementation->second->extendedType.get());
    if (extended == typeNodeTypes.end() || extended->second->kind != TypeRef::Kind::Named ||
        ParseTypeArgsFromTypeName(extended->second->name).empty()) {
        return std::nullopt;
    }
    return extended->second;
}

std::string AnalysisContext::MethodLinkerName(const FuncDecl &method, const TypeRef &receiverType,
                                              const std::unordered_map<std::string, TypeRef> &substitutions) const {
    const std::string typeName = NamedBaseTypeName(receiverType);
    std::string name = typeName + "::" + method.name;
    if (MethodIsOverloadedForIdentity(typeName, method.name)) {
        name += "__";
        bool first = true;
        // Methods of `extend Pair<int32>` and `extend Pair<int64>` may share a name and parameters, so a specialized
        // extension's own type leads the signature.
        if (const std::optional<TypeRef> specialized = SpecializedExtensionType(method)) {
            name += MangleTypeName(*specialized);
            first = false;
        }
        // A variadic parameter is part of the signature like any other, as a slice of its elements, so overloads that
        // differ only in what they collect are two symbols.
        for (const auto &parameter : method.params) {
            if (parameter.name == "self") {
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
    if (implementation != methodImpls.end() && !ImplTypeParams(*implementation->second).empty()) {
        if (const std::vector<TypeParameter> *typeParams = AggregateTypeParams(typeName)) {
            for (const auto &parameter : *typeParams) {
                if (const auto substitution = substitutions.find(parameter.name); substitution != substitutions.end()) {
                    name += "_" + MangleTypeName(substitution->second);
                }
            }
        }
    }
    for (const auto &parameter : method.typeParams) {
        if (const auto substitution = substitutions.find(parameter.name); substitution != substitutions.end()) {
            name += "_" + MangleTypeName(substitution->second);
        }
    }
    return name;
}

void AnalysisContext::RecordMethodIdentityRecipe(ResolvedCallableBinding &binding, const FuncDecl &method) const {
    assert(binding.receiverType && "method binding is missing its receiver type");
    std::string typeName = NamedBaseTypeName(*binding.receiverType);
    // A concrete extension names its methods after its own type, which the receiver need not spell the same way: a
    // writable view calls the methods of `extend int[..]`, and they are `int[..]::...` whoever calls them.
    if (const auto implementation = methodImpls.find(&method);
        implementation != methodImpls.end() && ImplTypeParams(*implementation->second).empty()) {
        if (const auto extended = typeNodeTypes.find(implementation->second->extendedType.get());
            extended != typeNodeTypes.end()) {
            typeName = NamedBaseTypeName(extended->second);
        }
    }
    binding.linkerNameBase = typeName + "::" + method.name;
    binding.linkerNameHasOverloadSignature = MethodIsOverloadedForIdentity(typeName, method.name);
    if (binding.linkerNameHasOverloadSignature) {
        if (const std::optional<TypeRef> specialized = SpecializedExtensionType(method)) {
            binding.linkerOverloadTypes.push_back(*specialized);
        }
        for (const auto &parameter : method.params) {
            if (parameter.name != "self") {
                binding.linkerOverloadTypes.push_back(IdentityParameterType(parameter, binding.substitutions));
            }
        }
    }

    const auto implementation = methodImpls.find(&method);
    if (implementation != methodImpls.end() && !ImplTypeParams(*implementation->second).empty()) {
        if (const std::vector<TypeParameter> *typeParams = AggregateTypeParams(typeName)) {
            binding.linkerSpecializationParameters = TypeParameterNames(*typeParams);
        }
    }
    AppendTypeParameterNames(binding.linkerSpecializationParameters, method.typeParams);
}

void AnalysisContext::BuildFinalSymbolIdentities() {
    for (const auto &[declaration, name] : programIndex.NominalNames()) {
        symbolIdentities.emplace(declaration, ResolvedSymbolIdentity{name});
    }
    for (const auto &[declaration, type] : checkedConstantTypes) {
        const auto &owner = declarationInfos.at(declaration);
        // Qualify even unique constants so an inlined initializer cannot collide with a caller's local binding.
        std::string identity = owner.ownerPackage + "::";
        if (!owner.modulePath.empty()) {
            identity += owner.modulePath + "::";
        }
        identity += declaration->name;
        symbolIdentities.emplace(declaration, ResolvedSymbolIdentity{std::move(identity)});
    }
    for (const auto &[declaration, type] : checkedGlobalTypes) {
        (void)type;
        const auto &owner = declarationInfos.at(declaration);
        // One storage location per variable across the whole program, so the name is qualified like a constant's.
        std::string identity = owner.ownerPackage + "::";
        if (!owner.modulePath.empty()) {
            identity += owner.modulePath + "::";
        }
        identity += declaration->name;
        symbolIdentities.emplace(declaration, ResolvedSymbolIdentity{std::move(identity)});
    }
    // Whether each function shares its name with another of its module, counted once per name: asking for every
    // function apart compares it with every function of its name, which is quadratic in a name many modules use.
    std::unordered_map<const FuncDecl *, bool> overloadedInModule;
    for (const auto &[name, declarations] : functionsByName) {
        (void)name;
        std::unordered_map<std::string_view, std::size_t> perModule;
        for (const auto *declaration : declarations) {
            ++perModule[functionModulePaths.at(declaration)];
        }
        for (const auto *declaration : declarations) {
            overloadedInModule.emplace(declaration, perModule.at(functionModulePaths.at(declaration)) > 1);
        }
    }
    const auto isOverloadedInModule = [&](const FuncDecl &declaration) {
        const auto known = overloadedInModule.find(&declaration);
        return known != overloadedInModule.end() ? known->second : FunctionIsOverloadedInModule(declaration);
    };
    const auto localFunctionName = [&](const FuncDecl &declaration) {
        return isOverloadedInModule(declaration) ? MangleFunctionWithParams(declaration) : declaration.name;
    };
    std::unordered_map<std::string, std::unordered_set<std::string_view>> owners;
    for (const auto &[name, declarations] : functionsByName) {
        (void)name;
        for (const auto *declaration : declarations) {
            owners[localFunctionName(*declaration)].insert(functionModulePaths.at(declaration));
        }
    }

    const auto qualifyFunctionName = [&](const FuncDecl &declaration, std::string name) {
        const auto *owner = programIndex.InfoFor(declaration);
        if (declaration.name == "Main" && owner) {
            if (owner->ownerPackage == packageName && owner->modulePath.empty())
                return name;
            return functionModulePaths.at(&declaration) + "::" + name;
        }
        const std::string &modulePath = functionModulePaths.at(&declaration);
        const auto local = owners.find(localFunctionName(declaration));
        if (local != owners.end() && local->second.size() > 1 && !modulePath.empty()) {
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
        if (!ImplTypeParams(*implementation).empty()) {
            continue;
        }
        const std::string typeName = BaseTypeName(implementation->typeName);
        const auto resolved = typeNodeTypes.find(implementation->extendedType.get());
        const TypeRef receiverType = resolved == typeNodeTypes.end() ? TypeRef::MakeNamed(typeName) : resolved->second;
        for (const auto &method : implementation->methods) {
            symbolIdentities.insert_or_assign(method.get(),
                                              ResolvedSymbolIdentity{MethodLinkerName(*method, receiverType, {})});
        }
        if (!implementation->interfaceName) {
            continue;
        }
        const auto *owner = programIndex.InfoFor(*implementation);
        const Symbol *symbol = owner && owner->scope ? owner->scope->Lookup(*implementation->interfaceName) : nullptr;
        const std::string interfaceName = symbol && programIndex.NominalNames().contains(symbol->declaration)
                                            ? programIndex.NominalName(*symbol->declaration)
                                            : *implementation->interfaceName;
        const auto interface = interfaceDecls.find(interfaceName);
        // A generic interface has no value form, so nothing ever dispatches through a table of its operations.
        if (interface == interfaceDecls.end() || interface->second->methods.empty() ||
            !interface->second->typeParams.empty()) {
            continue;
        }
        // The vtable and its entries name the receiver the way its methods do, so an extension written on a spelling
        // of a slice conforms the slice rather than a type of that spelling, and every entry names a method that
        // exists.
        const std::string receiverName = NamedBaseTypeName(receiverType);
        ResolvedVtableIdentity identity;
        identity.interfaceName = interfaceName;
        identity.linkerName = "__vtable__" + receiverName + "__" + interfaceName;
        for (const auto &method : interface->second->methods) {
            identity.entries.push_back(receiverName + "::" + method->name);
        }
        vtableIdentities.insert_or_assign(implementation, std::move(identity));
    }

    // Every binding is named on its own from what is settled above, so the bindings are spread over the workers.
    const auto forEachBinding = [this]<typename Map>(Map &bindings, const auto &name) {
        std::vector<typename Map::mapped_type *> pending;
        pending.reserve(bindings.size());
        for (auto &[_, binding] : bindings)
            pending.push_back(&binding);
        constexpr std::size_t chunk = 512;
        const std::size_t chunks = (pending.size() + chunk - 1) / chunk;
        const auto run = [&](const std::size_t index) {
            const std::size_t end = std::min(pending.size(), (index + 1) * chunk);
            for (std::size_t item = index * chunk; item < end; ++item)
                name(*pending[item]);
        };
        if (workers != nullptr && chunks > 1) {
            workers->ForEach(chunks, run);
        }
        else {
            for (std::size_t index = 0; index < chunks; ++index)
                run(index);
        }
    };
    forEachBinding(functionReferenceBindings, [&](ResolvedCallableBinding &binding) {
        const auto &function = static_cast<const FuncDecl &>(*binding.selectedDeclaration);
        binding.linkerNameBase = qualifyFunctionName(function, function.name);
        binding.linkerNameHasOverloadSignature = isOverloadedInModule(function);
        if (binding.linkerNameHasOverloadSignature) {
            for (const auto &parameter : function.params) {
                binding.linkerOverloadTypes.push_back(IdentityParameterType(parameter, binding.substitutions));
            }
        }
        binding.linkerSpecializationParameters = TypeParameterNames(function.typeParams);
        binding.linkerName = binding.LinkerNameFor(binding.substitutions);
    });
    forEachBinding(callableBindings, [&](ResolvedCallableBinding &binding) {
        if (!binding.selectedDeclaration || binding.dispatch == ResolvedCallableBinding::DispatchKind::Interface ||
            binding.dispatch == ResolvedCallableBinding::DispatchKind::Indirect ||
            binding.dispatch == ResolvedCallableBinding::DispatchKind::EnumVariant ||
            // A constrained call has one target per instantiation; lowering names it from the witness instead.
            binding.dispatch == ResolvedCallableBinding::DispatchKind::Constrained) {
            return;
        }
        const auto *function = AstCast<const FuncDecl>(binding.selectedDeclaration);
        if (function &&
            (binding.dispatch == ResolvedCallableBinding::DispatchKind::Method ||
             binding.dispatch == ResolvedCallableBinding::DispatchKind::Constructor) &&
            binding.receiverType) {
            binding.linkerName = MethodLinkerName(*function, *binding.receiverType, binding.substitutions);
            RecordMethodIdentityRecipe(binding, *function);
            return;
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
            binding.linkerNameHasOverloadSignature = isOverloadedInModule(*function);
            if (binding.linkerNameHasOverloadSignature) {
                for (const auto &parameter : function->params) {
                    if (!parameter.isVariadic) {
                        binding.linkerOverloadTypes.push_back(IdentityParameterType(parameter, binding.substitutions));
                    }
                }
            }
            binding.linkerSpecializationParameters = TypeParameterNames(function->typeParams);
            binding.linkerName = binding.LinkerNameFor(binding.substitutions);
            return;
        }
        if (const auto identity = symbolIdentities.find(binding.selectedDeclaration);
            identity != symbolIdentities.end()) {
            binding.linkerName = identity->second.linkerName;
        }
    });
}

// Expressions
TypeRef AnalysisContext::CheckExpr(const Expr &expr) {
    const std::size_t diagnosticStart = diags.size();
    TypeRef type = CheckExprImpl(expr);
    if (const auto *call = AstCast<const CallExpr>(&expr)) {
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
