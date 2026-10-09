// Associated constants: `Type::Name` for a declared type, and for a built-in primitive the constants the packages in
// scope declare in their `extend` blocks for it.

#include "Semantic/Analysis/AnalysisContext.h"
#include "Types/PrimitiveCatalog.h"

#include <format>
#include <map>
#include <set>

namespace Rux::SemanticDetail {
bool AnalysisContext::IsVisibleTypeSymbol(const Symbol &symbol) const {
    if (!symbol.declaration) {
        return false;
    }
    if (symbol.ownerPackage == currentPackage) {
        return true;
    }
    const auto imported = explicitTypeImports.find(currentFile);
    return imported != explicitTypeImports.end() && imported->second.contains(symbol.declaration);
}

std::optional<TypeRef::Kind> AnalysisContext::PrimitiveProviders(const Symbol &symbol,
                                                                 std::unordered_set<std::string> &providers) const {
    std::vector<std::string> files{currentFile};
    std::unordered_set<const Decl *> visited;
    std::optional<TypeRef::Kind> kind;
    for (const Symbol *candidate = &symbol; candidate;) {
        if (candidate->ownerPackage == "<builtin>") {
            if (const PrimitiveInfo *primitive = FindPrimitive(candidate->name)) {
                kind = primitive->kind;
            }
            break;
        }
        const auto *alias = dynamic_cast<const TypeAliasDecl *>(candidate->declaration);
        const auto *named = alias ? dynamic_cast<const NamedTypeExpr *>(alias->type.get()) : nullptr;
        const auto info = alias ? declarationInfos.find(alias) : declarationInfos.end();
        if (!named || !named->typeArgs.empty() || info == declarationInfos.end() || !info->second.scope ||
            !visited.insert(alias).second) {
            break;
        }
        // An alias carries the extensions in force where it is declared, so `Tiny::Max` works wherever `Tiny` is
        // imported, whether or not the importer also imported an extension of `int8`.
        providers.insert(info->second.ownerPackage);
        files.push_back(info->second.sourceName);
        candidate = info->second.scope->Lookup(named->name);
    }
    if (!kind) {
        return std::nullopt;
    }
    providers.insert(currentPackage);
    for (const std::string &file : files) {
        const auto fileImports = primitiveExtensionImports.find(file);
        if (fileImports == primitiveExtensionImports.end()) {
            continue;
        }
        if (const auto packages = fileImports->second.find(*kind); packages != fileImports->second.end()) {
            providers.insert(packages->second.begin(), packages->second.end());
        }
    }
    return kind;
}

AnalysisContext::AssociatedConstantLookup AnalysisContext::LookupAssociatedConstant(const Symbol &type,
                                                                                    const std::string &name) const {
    // A built-in name is visible everywhere; a declared type, alias included, only where it is declared or imported.
    if (type.ownerPackage != "<builtin>" && !IsVisibleTypeSymbol(type)) {
        return {};
    }
    std::unordered_set<std::string> providers;
    if (const auto kind = PrimitiveProviders(type, providers)) {
        // Sorted by package, so a conflict lists its packages in the same order every time.
        std::map<std::string, const ConstDecl *> declared;
        for (const ImplDecl *implementation : implDecls) {
            const auto owner = declarationInfos.find(implementation);
            const PrimitiveInfo *primitive = FindPrimitive(implementation->typeName);
            if (owner == declarationInfos.end() || !providers.contains(owner->second.ownerPackage) || !primitive ||
                primitive->kind != *kind) {
                continue;
            }
            for (const auto &constant : implementation->constants) {
                if (constant->name == name) {
                    declared.emplace(owner->second.ownerPackage, constant.get());
                }
            }
        }
        AssociatedConstantLookup result;
        if (declared.size() > 1) {
            for (const auto &[package, unused] : declared) {
                (void)unused;
                result.conflictingPackages.push_back(PackageDisplayName(package));
            }
        }
        else if (!declared.empty()) {
            result.constant = declared.begin()->second;
        }
        return result;
    }
    for (const ImplDecl *implementation : implDecls) {
        const auto owner = declarationInfos.find(implementation);
        if (owner == declarationInfos.end() || owner->second.ownerPackage != type.ownerPackage ||
            implementation->typeName != type.name) {
            continue;
        }
        for (const auto &constant : implementation->constants) {
            if (constant->name == name) {
                return {constant.get(), {}};
            }
        }
    }
    return {};
}

std::optional<std::string> AnalysisContext::AssociatedConstantImportHelp(const Symbol &type,
                                                                         const std::string &name) const {
    std::unordered_set<std::string> providers;
    const auto kind = PrimitiveProviders(type, providers);
    if (!kind) {
        return std::nullopt;
    }
    // The name this package imports a dependency by, or nullopt when it is not a direct dependency.
    const auto importName = [&](const std::string &package) -> std::optional<std::string> {
        const auto own = imports.find(currentPackage);
        if (own == imports.end()) {
            return package;
        }
        for (const auto &[alias, identity] : own->second) {
            if (identity == package) {
                return alias;
            }
        }
        return std::nullopt;
    };
    // Name whichever dependency actually declares the constant, never a package by its identity.
    std::set<std::string> candidates;
    for (const ImplDecl *implementation : implDecls) {
        const auto owner = declarationInfos.find(implementation);
        const PrimitiveInfo *primitive = FindPrimitive(implementation->typeName);
        if (owner == declarationInfos.end() || providers.contains(owner->second.ownerPackage) || !primitive ||
            primitive->kind != *kind) {
            continue;
        }
        for (const auto &constant : implementation->constants) {
            if (constant->name != name || !constant->isPublic) {
                continue;
            }
            if (const auto alias = importName(owner->second.ownerPackage)) {
                candidates.insert(*alias);
            }
        }
    }
    if (candidates.empty()) {
        return std::nullopt;
    }
    return std::format("import the extension that declares it, for example 'import {}::{};'", *candidates.begin(),
                       type.name);
}

void AnalysisContext::ImportPrimitiveExtension(const UseDecl &declaration, const ImportScope &scope,
                                               const PrimitiveInfo &primitive, const std::string &name) {
    const auto *extended = programIndex.PrimitiveExtensions(scope.ownerPackage, scope.modulePath);
    if (!extended || !extended->contains(primitive.kind)) {
        std::vector<std::string> notes;
        if (!primitive.implemented) {
            notes.push_back(std::format(
                "primitive type '{}' is reserved but is not implemented in this compiler version", primitive.name));
        }
        EmitError(declaration.location,
                  std::format("{} declares no extension of primitive type '{}'", scope.displayName, name),
                  std::move(notes),
                  std::format("a primitive type needs no import; import it only from a package that declares "
                              "'extend {} {{ ... }}'",
                              primitive.name));
        return;
    }
    primitiveExtensionImports[currentFile][primitive.kind].insert(scope.ownerPackage);
}
} // namespace Rux::SemanticDetail
