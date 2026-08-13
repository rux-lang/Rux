#include "Semantic/SemanticModel.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdlib>
#include <utility>

namespace Rux {
namespace {
bool IdentityCharacter(const char character) {
    return std::isalnum(static_cast<unsigned char>(character)) || character == '_';
}

std::string SubstituteIdentityName(std::string name, const std::unordered_map<std::string, TypeRef> &substitutions) {
    for (const auto &[parameter, type] : substitutions) {
        std::size_t position = 0;
        while ((position = name.find(parameter, position)) != std::string::npos) {
            const bool beginsIdentifier = position != 0 && IdentityCharacter(name[position - 1]);
            const std::size_t end = position + parameter.size();
            const bool endsIdentifier = end < name.size() && IdentityCharacter(name[end]);
            if (beginsIdentifier || endsIdentifier) {
                position = end;
                continue;
            }
            const std::string replacement = type.ToString();
            name.replace(position, parameter.size(), replacement);
            position += replacement.size();
        }
    }
    return name;
}

TypeRef SubstituteIdentityType(TypeRef type, const std::unordered_map<std::string, TypeRef> &substitutions) {
    if (type.kind == TypeRef::Kind::TypeParam || type.kind == TypeRef::Kind::Named) {
        if (const auto substitution = substitutions.find(type.name); substitution != substitutions.end()) {
            return substitution->second;
        }
        if (type.kind == TypeRef::Kind::Named) {
            type.name = SubstituteIdentityName(std::move(type.name), substitutions);
        }
    }
    for (auto &inner : type.inner) {
        inner = SubstituteIdentityType(std::move(inner), substitutions);
    }
    return type;
}

std::string MangleIdentityType(const TypeRef &type) {
    std::string name;
    for (const char character : type.ToString()) {
        name += IdentityCharacter(character) ? character : '_';
    }
    return name.empty() ? "_" : name;
}

const TypeRef &RequireSubstitution(const std::unordered_map<std::string, TypeRef> &substitutions,
                                   const std::string &parameter) {
    const auto substitution = substitutions.find(parameter);
    assert(substitution != substitutions.end() && "concrete call identity is missing a semantic substitution");
    if (substitution == substitutions.end()) {
        std::abort();
    }
    return substitution->second;
}
} // namespace

std::string
ResolvedCallableBinding::LinkerNameFor(const std::unordered_map<std::string, TypeRef> &concreteSubstitutions) const {
    if (linkerNameBase.empty()) {
        return linkerName;
    }

    std::string name = linkerNameBase;
    if (linkerNameHasOverloadSignature) {
        name += "__";
        for (std::size_t i = 0; i < linkerOverloadTypes.size(); ++i) {
            if (i != 0) {
                name += "_";
            }
            name += MangleIdentityType(SubstituteIdentityType(linkerOverloadTypes[i], concreteSubstitutions));
        }
    }
    for (const auto &parameter : linkerSpecializationParameters) {
        name += "_" + MangleIdentityType(RequireSubstitution(concreteSubstitutions, parameter));
    }
    return name;
}

ResolvedCallableBinding
ResolvedCallableBinding::Instantiate(const std::unordered_map<std::string, TypeRef> &contextSubstitutions) const {
    ResolvedCallableBinding concrete = *this;
    for (auto &[_, type] : concrete.substitutions) {
        type = SubstituteIdentityType(std::move(type), contextSubstitutions);
    }
    if (concrete.receiverType) {
        concrete.receiverType = SubstituteIdentityType(std::move(*concrete.receiverType), contextSubstitutions);
    }
    concrete.linkerName = LinkerNameFor(concrete.substitutions);
    return concrete;
}

SemanticModel::SemanticModel(std::vector<SemanticDiagnostic> inputDiagnostics, std::vector<SemanticSymbol> inputSymbols,
                             std::vector<const Module *> inputModules, CompileTimeContext inputCompileTimeContext,
                             std::unordered_map<const Expr *, TypeRef> inputExpressionTypes,
                             std::unordered_map<const TypeExpr *, TypeRef> inputTypeNodeTypes,
                             std::unordered_map<const Pattern *, TypeRef> inputPatternTypes,
                             std::unordered_map<const CallExpr *, ResolvedCallableBinding> inputCallableBindings,
                             std::unordered_map<const Decl *, ResolvedSymbolIdentity> inputSymbolIdentities,
                             std::unordered_map<const ImplDecl *, ResolvedVtableIdentity> inputVtableIdentities,
                             std::unordered_map<std::string, ResolvedTypeLayout> inputTypeLayouts,
                             std::unordered_map<const SizeOfExpr *, std::uint64_t> inputSizeOfValues)
    : diagnostics(std::move(inputDiagnostics))
    , symbols(std::move(inputSymbols))
    , modules(std::move(inputModules))
    , compileTimeContext(std::move(inputCompileTimeContext))
    , expressionTypes(std::move(inputExpressionTypes))
    , typeNodeTypes(std::move(inputTypeNodeTypes))
    , patternTypes(std::move(inputPatternTypes))
    , callableBindings(std::move(inputCallableBindings))
    , symbolIdentities(std::move(inputSymbolIdentities))
    , vtableIdentities(std::move(inputVtableIdentities))
    , typeLayouts(std::move(inputTypeLayouts))
    , sizeOfValues(std::move(inputSizeOfValues)) {
}

bool SemanticModel::HasErrors() const noexcept {
    return std::ranges::any_of(
        diagnostics, [](const SemanticDiagnostic &d) { return d.severity == SemanticDiagnostic::Severity::Error; });
}

const TypeRef *SemanticModel::TryGetType(const Expr &expression) const noexcept {
    const auto type = expressionTypes.find(&expression);
    return type == expressionTypes.end() ? nullptr : &type->second;
}

const TypeRef *SemanticModel::TryGetType(const TypeExpr &typeNode) const noexcept {
    const auto type = typeNodeTypes.find(&typeNode);
    return type == typeNodeTypes.end() ? nullptr : &type->second;
}

const TypeRef *SemanticModel::TryGetType(const Pattern &pattern) const noexcept {
    const auto type = patternTypes.find(&pattern);
    return type == patternTypes.end() ? nullptr : &type->second;
}

const ResolvedCallableBinding *SemanticModel::TryGetCallableBinding(const CallExpr &call) const noexcept {
    const auto binding = callableBindings.find(&call);
    return binding == callableBindings.end() ? nullptr : &binding->second;
}

const ResolvedSymbolIdentity *SemanticModel::TryGetSymbolIdentity(const Decl &declaration) const noexcept {
    const auto identity = symbolIdentities.find(&declaration);
    return identity == symbolIdentities.end() ? nullptr : &identity->second;
}

const ResolvedVtableIdentity *SemanticModel::TryGetVtableIdentity(const ImplDecl &declaration) const noexcept {
    const auto identity = vtableIdentities.find(&declaration);
    return identity == vtableIdentities.end() ? nullptr : &identity->second;
}

const ResolvedTypeLayout *SemanticModel::TryGetLayout(const TypeRef &type) const noexcept {
    const auto layout = typeLayouts.find(type.ToString());
    return layout == typeLayouts.end() ? nullptr : &layout->second;
}

const ResolvedTypeLayout *SemanticModel::TryGetLayout(const TypeExpr &typeNode) const noexcept {
    const TypeRef *type = TryGetType(typeNode);
    return type ? TryGetLayout(*type) : nullptr;
}

const std::uint64_t *SemanticModel::TryGetSizeOfValue(const SizeOfExpr &expression) const noexcept {
    const auto value = sizeOfValues.find(&expression);
    return value == sizeOfValues.end() ? nullptr : &value->second;
}
} // namespace Rux
