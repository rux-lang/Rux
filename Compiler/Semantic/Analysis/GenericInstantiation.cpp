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

void AnalysisContext::QueueGenericInstantiation(const FuncDecl &decl,
                                                const std::unordered_map<std::string, TypeRef> &substitutions) {
    if (substitutions.empty()) {
        return;
    }
    // A generic function carries its own parameters; a method of a generic type carries none of its own and is
    // instantiated by what the receiver's type arguments say. Both are instantiations with a body to re-check,
    // so the concreteness test reads the substitution map rather than the declaration.
    if (!decl.typeParams.empty() && substitutions.size() != decl.typeParams.size()) {
        return;
    }

    const bool isConcrete = std::ranges::all_of(substitutions, [this](const auto &substitution) {
        return !substitution.second.IsUnknown() && !MentionsTypeParameter(substitution.second);
    });
    if (!isConcrete) {
        if (currentFunctionDecl) {
            deferredGenericCalls[currentFunctionDecl].push_back({&decl, substitutions});
        }
        return;
    }
    pendingGenericInstantiations.push_back({&decl, substitutions});
}

void AnalysisContext::QueueDropMethodInstantiations() {
    const auto queue = [&](const TypeRef &type) {
        if (type.IsUnknown() || MentionsTypeParameter(type) || !ClassifyTypeProperties(type).IsDroppable()) {
            return;
        }
        const std::string destructorName = "~" + NamedBaseTypeName(type);
        const FuncDecl *destructor = LookupMethod(type, destructorName, {}, false);
        if (destructor == nullptr) {
            return;
        }
        QueueGenericInstantiation(*destructor, MethodTypeSubstitutions(type));
    };
    for (const auto &[expression, type] : expressionTypes) {
        queue(type);
    }
    for (const auto &[node, type] : typeNodeTypes) {
        queue(type);
    }
    for (const auto &[pattern, type] : patternTypes) {
        queue(type);
    }
}

void AnalysisContext::ValidatePendingGenericInstantiations() {
    std::size_t processed = 0;
    while (processed < pendingGenericInstantiations.size()) {
        PendingGenericInstantiation instantiation = std::move(pendingGenericInstantiations[processed++]);

        // Keyed by what the parameters stand for, taken from the substitution map in a fixed order so a method
        // of a generic type -- which has no parameters of its own to walk -- is deduplicated too.
        std::vector<std::string> names;
        names.reserve(instantiation.substitutions.size());
        for (const auto &[name, type] : instantiation.substitutions) {
            names.push_back(name);
        }
        std::ranges::sort(names);
        std::string key;
        for (const std::string &name : names) {
            if (!key.empty()) {
                key += ";";
            }
            key += name + "=" + instantiation.substitutions.at(name).ToString();
        }
        if (!validatedGenericInstantiations[instantiation.decl].insert(std::move(key)).second) {
            continue;
        }

        // Refusing to process this instantiation is what stops the runaway: its body is never walked, so it never
        // queues the next, larger one. Reported once per function, since every instantiation past the limit has
        // the same cause and listing them would bury it.
        const auto oversized = std::ranges::find_if(instantiation.substitutions, [](const auto &substitution) {
            return TypeNodeCount(substitution.second, kMaxInstantiationTypeNodes) > kMaxInstantiationTypeNodes;
        });
        if (oversized != instantiation.substitutions.end()) {
            if (reportedRunawayInstantiations.insert(instantiation.decl).second) {
                // The offending type is by definition enormous, and printing all of it would bury the message
                // it is meant to illustrate.
                constexpr std::size_t kShownTypeCharacters = 40;
                std::string shown = oversized->second.ToString();
                if (shown.size() > kShownTypeCharacters) {
                    shown = shown.substr(0, kShownTypeCharacters) + "...";
                }
                EmitError(
                    instantiation.decl->location,
                    std::format("generic function '{}' instantiates itself without end", instantiation.decl->name),
                    {std::format("type argument '{}' for '{}' grew past the limit of {} type nodes", shown,
                                 oversized->first, kMaxInstantiationTypeNodes)},
                    "give the recursion a case that stops, or one that reuses a type argument it has "
                    "already been given");
            }
            continue;
        }

        Scope *savedScope = currentScope;
        const std::string savedFile = currentFile;
        const std::string savedPackage = currentPackage;
        const FuncDecl *savedFunctionDecl = currentFunctionDecl;
        if (const auto it = functionDeclScopes.find(instantiation.decl); it != functionDeclScopes.end()) {
            currentScope = it->second;
        }
        if (const auto it = functionDeclFiles.find(instantiation.decl); it != functionDeclFiles.end()) {
            currentFile = it->second;
        }
        if (const auto it = declarationInfos.find(instantiation.decl); it != declarationInfos.end()) {
            currentPackage = it->second.ownerPackage;
        }
        currentFunctionDecl = nullptr;

        // A type argument that is itself an instantiation -- `AllocateBlock<Node<int32>>` -- is named for the
        // first time right here. Nothing in the source spells it, so nothing computed its layout, and the
        // `sizeof` this instantiation folds would have had nothing to read. Laying it out now is what puts it
        // in the model that lowering asks.
        for (const auto &substitution : instantiation.substitutions) {
            LayoutOfTypeRef(substitution.second);
        }

        if (const auto it = deferredEnumInstantiations.find(instantiation.decl);
            it != deferredEnumInstantiations.end()) {
            for (const DeferredEnumInstantiation &deferred : it->second) {
                std::vector<TypeRef> arguments;
                arguments.reserve(deferred.typeArgs.size());
                for (const TypeRef &argument : deferred.typeArgs) {
                    arguments.push_back(SubstituteTypeParameters(argument, instantiation.substitutions));
                }
                instantiatedTypes.push_back(EnumType(*deferred.decl, arguments));
            }
        }

        ValidateDeferredBasicExpressionChecks(*instantiation.decl, instantiation.substitutions);
        if (const auto it = deferredGenericCalls.find(instantiation.decl); it != deferredGenericCalls.end()) {
            for (const DeferredGenericCall &call : it->second) {
                std::unordered_map<std::string, TypeRef> substitutions;
                for (const auto &[param, type] : call.substitutions) {
                    substitutions.emplace(param, SubstituteTypeParameters(type, instantiation.substitutions));
                }
                QueueGenericInstantiation(*call.callee, substitutions);
            }
        }

        currentFunctionDecl = savedFunctionDecl;
        currentPackage = savedPackage;
        currentFile = savedFile;
        currentScope = savedScope;
    }
}
} // namespace Rux::SemanticDetail
