#include "Semantic/SemanticModel.h"

#include <algorithm>
#include <utility>

namespace Rux {
SemanticModel::SemanticModel(std::vector<SemanticDiagnostic> inputDiagnostics, std::vector<SemanticSymbol> inputSymbols,
                             std::vector<const Module *> inputModules, CompileTimeContext inputCompileTimeContext,
                             std::unordered_map<const Expr *, TypeRef> inputExpressionTypes,
                             std::unordered_map<const TypeExpr *, TypeRef> inputTypeNodeTypes,
                             std::unordered_map<const Pattern *, TypeRef> inputPatternTypes,
                             std::unordered_map<const CallExpr *, ResolvedCallableBinding> inputCallableBindings,
                             std::unordered_map<const Decl *, ResolvedSymbolIdentity> inputSymbolIdentities,
                             std::unordered_map<const ImplDecl *, ResolvedVtableIdentity> inputVtableIdentities)
    : diagnostics(std::move(inputDiagnostics))
    , symbols(std::move(inputSymbols))
    , modules(std::move(inputModules))
    , compileTimeContext(std::move(inputCompileTimeContext))
    , expressionTypes(std::move(inputExpressionTypes))
    , typeNodeTypes(std::move(inputTypeNodeTypes))
    , patternTypes(std::move(inputPatternTypes))
    , callableBindings(std::move(inputCallableBindings))
    , symbolIdentities(std::move(inputSymbolIdentities))
    , vtableIdentities(std::move(inputVtableIdentities)) {
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
} // namespace Rux
