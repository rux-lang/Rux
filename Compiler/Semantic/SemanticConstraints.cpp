// Generic constraint resolution: what an interface bound written on a type parameter means, and whether the type
// argument a call or a type reference supplies actually provides the operations that bound promises.

#include "Semantic/Detail/SemanticAnalyzerContext.h"

#include <format>
#include <unordered_set>
#include <utility>

namespace Rux::SemanticDetail {
namespace {
/// The bound as the reader wrote it. A bound is a plain interface name, so anything with a type argument list or a
/// module path is reported at its own spelling rather than silently reduced to the last segment.
[[nodiscard]] const NamedTypeExpr *BoundName(const TypeExpr &bound) {
    return dynamic_cast<const NamedTypeExpr *>(&bound);
}
} // namespace

std::vector<SemanticAnalyzerContext::ResolvedTypeBound>
SemanticAnalyzerContext::ResolveTypeParameterBounds(const TypeParameter &parameter, const bool report) {
    std::vector<ResolvedTypeBound> resolved;
    std::unordered_set<std::string> seen;
    for (const TypeExprPtr &bound : parameter.bounds) {
        const NamedTypeExpr *named = BoundName(*bound);
        if (!named) {
            if (report) {
                EmitError(bound->location,
                          std::format("bound on type parameter '{}' must name an interface", parameter.name));
            }
            continue;
        }
        if (!named->typeArgs.empty()) {
            if (report) {
                EmitError(named->location, std::format("interface bound '{}' cannot take type arguments", named->name));
            }
            resolved.push_back({named->name, nullptr, named->location});
            continue;
        }

        const Symbol *symbol = currentScope->Lookup(named->name);
        if (!symbol) {
            if (report) {
                std::optional<std::string> help;
                if (const Symbol *suggestion = currentScope->Suggest(named->name)) {
                    help = std::format("did you mean '{}'?", suggestion->name);
                }
                EmitError(named->location, std::format("interface '{}' is not defined", named->name), {},
                          std::move(help));
            }
            resolved.push_back({named->name, nullptr, named->location});
            continue;
        }
        if (symbol->kind != Symbol::Kind::Interface) {
            if (report) {
                EmitError(named->location,
                          std::format("name '{}' is a {}, not an interface, and cannot bound type parameter '{}'",
                                      named->name, SymbolKindName(symbol->kind), parameter.name),
                          {DeclarationNote(*symbol)});
            }
            resolved.push_back({named->name, nullptr, named->location});
            continue;
        }
        if (!seen.insert(named->name).second) {
            if (report) {
                EmitError(named->location,
                          std::format("type parameter '{}' repeats interface bound '{}'", parameter.name, named->name));
            }
            continue;
        }

        const auto declaration = interfaceDecls.find(named->name);
        resolved.push_back(
            {named->name, declaration == interfaceDecls.end() ? nullptr : declaration->second, named->location});
    }
    return resolved;
}

void SemanticAnalyzerContext::DeclareTypeParameterBounds(const std::vector<TypeParameter> &parameters) {
    for (const TypeParameter &parameter : parameters) {
        std::vector<ResolvedTypeBound> bounds = ResolveTypeParameterBounds(parameter, /*report=*/true);
        currentTypeParamBounds[parameter.name] = std::move(bounds);
    }
}

SemanticAnalyzerContext::ScopedTypeParameterBounds::ScopedTypeParameterBounds(
    SemanticAnalyzerContext &owner, const std::vector<TypeParameter> *parameters, const bool replaceEnclosing)
    : context(owner)
    , saved(owner.currentTypeParamBounds) {
    if (replaceEnclosing) {
        context.currentTypeParamBounds.clear();
    }
    if (parameters) {
        context.DeclareTypeParameterBounds(*parameters);
    }
}

SemanticAnalyzerContext::ScopedTypeParameterBounds::~ScopedTypeParameterBounds() {
    context.currentTypeParamBounds = std::move(saved);
}

bool SemanticAnalyzerContext::TypeSatisfiesBound(const TypeRef &argument, const InterfaceDecl &interface,
                                                 std::string &reason) {
    // An interface that requires nothing is satisfied by everything, so a marker bound costs no conformance work.
    if (interface.methods.empty()) {
        return true;
    }

    // A type parameter passed on as a type argument carries only what its own declaration promised. Checking that
    // promise here rather than at each instantiation is what keeps a generic body checkable on its own: the caller
    // already had to satisfy the outer bound, so the inner one follows.
    if (argument.kind == TypeRef::Kind::TypeParam) {
        const auto bounds = currentTypeParamBounds.find(argument.name);
        // A parameter with no recorded bounds belongs to a declaration that is not the one being checked -- a signature
        // is resolved before its body, and a type is read back in contexts that carry names but no constraints. What
        // that parameter promised is unknown here, so the pass over its own declaration decides instead of this one.
        if (bounds == currentTypeParamBounds.end()) {
            return true;
        }
        for (const ResolvedTypeBound &bound : bounds->second) {
            if (bound.interface == &interface) {
                return true;
            }
        }
        reason = std::format("type parameter '{}' is not constrained by '{}'", argument.name, interface.name);
        return false;
    }

    const std::string typeName = NamedBaseTypeName(argument);
    if (typeName.empty()) {
        reason = std::format("type '{}' cannot carry the methods interface '{}' requires", argument.ToString(),
                             interface.name);
        return false;
    }

    // `extend T: I` states the conformance outright. Without one, the methods themselves are what a constrained
    // generic actually calls, so a type that declares every required method satisfies the bound as well: the call is
    // resolved statically, and demanding a nominal declaration would reject code that compiles either way.
    if (const auto implemented = typeImplementsInterfaces.find(typeName);
        implemented != typeImplementsInterfaces.end() && implemented->second.contains(interface.name)) {
        return true;
    }

    const auto methods = methodsByType.find(typeName);
    for (const auto &required : interface.methods) {
        if (methods == methodsByType.end() || !methods->second.contains(required->name)) {
            reason = std::format("interface '{}' requires method '{}', which type '{}' does not implement",
                                 interface.name, required->name, argument.ToString());
            return false;
        }
    }
    return true;
}

void SemanticAnalyzerContext::CheckTypeArgumentConstraints(
    const std::vector<TypeParameter> &parameters, const std::unordered_map<std::string, TypeRef> &substitutions,
    const SourceLocation location, const std::string &subject) {
    for (const TypeParameter &parameter : parameters) {
        if (parameter.bounds.empty()) {
            continue;
        }
        const auto argument = substitutions.find(parameter.name);
        if (argument == substitutions.end() || argument->second.IsUnknown()) {
            continue;
        }

        for (const ResolvedTypeBound &bound : ResolveTypeParameterBounds(parameter, /*report=*/false)) {
            // A bound that named nothing usable was reported where it was written; repeating it at each use would bury
            // the one diagnostic that can be acted on.
            if (!bound.interface) {
                continue;
            }
            std::string reason;
            if (TypeSatisfiesBound(argument->second, *bound.interface, reason)) {
                continue;
            }
            const std::string argumentName = argument->second.ToString();
            std::vector<std::string> notes{std::move(reason)};
            notes.push_back(
                std::format("type parameter '{}' of {} is bound by '{}'", parameter.name, subject, bound.name));
            std::optional<std::string> help;
            if (argument->second.kind == TypeRef::Kind::TypeParam) {
                help =
                    std::format("add the bound to the enclosing declaration, as in '{}: {}'", argumentName, bound.name);
            }
            else {
                help =
                    std::format("implement the interface, as in 'extend {}: {} {{ ... }}'", argumentName, bound.name);
            }
            EmitError(location,
                      std::format("type argument '{}' does not satisfy interface bound '{}' on type parameter '{}'",
                                  argumentName, bound.name, parameter.name),
                      std::move(notes), std::move(help));
        }
    }
}

void SemanticAnalyzerContext::CheckTypeReferenceConstraints(const TypeExpr &expression,
                                                            const std::vector<TypeParameter> &parameters,
                                                            const std::vector<TypeRef> &typeArguments,
                                                            const std::string &subject) {
    if (parameters.size() != typeArguments.size() || reportedTypeArgumentConstraints.contains(&expression)) {
        return;
    }
    std::unordered_map<std::string, TypeRef> substitutions;
    for (std::size_t index = 0; index < parameters.size(); ++index) {
        substitutions.emplace(parameters[index].name, typeArguments[index]);
    }
    // Only a report consumes the spelling. A pass that reached this type before its enclosing declaration's bounds were
    // recorded decided nothing, and suppressing the later pass on the strength of it would lose the diagnostic.
    const std::size_t before = diags.size();
    CheckTypeArgumentConstraints(parameters, substitutions, expression.location, subject);
    if (diags.size() != before) {
        reportedTypeArgumentConstraints.insert(&expression);
    }
}

void SemanticAnalyzerContext::CheckWrittenTypeArgumentConstraints(const std::vector<TypeParameter> &parameters,
                                                                  const std::vector<TypeExprPtr> &typeArguments,
                                                                  const SourceLocation location,
                                                                  const std::string &subject) {
    if (parameters.size() != typeArguments.size()) {
        return;
    }
    std::unordered_map<std::string, TypeRef> substitutions;
    for (std::size_t index = 0; index < parameters.size(); ++index) {
        substitutions.emplace(parameters[index].name, ResolveType(*typeArguments[index]));
    }
    CheckTypeArgumentConstraints(parameters, substitutions, location, subject);
}

bool SemanticAnalyzerContext::TypeArgumentsSatisfyBounds(
    const std::vector<TypeParameter> &parameters, const std::unordered_map<std::string, TypeRef> &substitutions) {
    for (const TypeParameter &parameter : parameters) {
        if (parameter.bounds.empty()) {
            continue;
        }
        const auto argument = substitutions.find(parameter.name);
        if (argument == substitutions.end() || argument->second.IsUnknown()) {
            continue;
        }
        for (const ResolvedTypeBound &bound : ResolveTypeParameterBounds(parameter, /*report=*/false)) {
            std::string reason;
            if (bound.interface && !TypeSatisfiesBound(argument->second, *bound.interface, reason)) {
                return false;
            }
        }
    }
    return true;
}
} // namespace Rux::SemanticDetail
