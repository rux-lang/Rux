#include "Lexer/Lexer.h"
#include "Numeric/IntegerLiteral.h"
#include "Semantic/Analysis/AnalysisContext.h"
#include "Semantic/Conditional/ConditionalCompilation.h"
#include "Target/Layout.h"
#include "Target/Target.h"
#include "Types/PrimitiveCatalog.h"
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

void AnalysisContext::ApplyModuleImports(const Module &mod) {
    currentFile = mod.name;
    for (const auto &decl : mod.items) {
        ApplyDeclImports(*decl);
    }
}

void AnalysisContext::ApplyModuleImportsInScope(const Module &mod, Scope &scope) {
    Scope *savedScope = currentScope;
    currentScope = &scope;
    ApplyModuleImports(mod);
    currentScope = savedScope;
}

void AnalysisContext::ApplyDeclImports(const Decl &decl) {
    if (auto *useDecl = dynamic_cast<const UseDecl *>(&decl)) {
        CheckUseDecl(*useDecl);
        appliedImports.insert(useDecl);
    }
    else if (auto *modDecl = dynamic_cast<const ModuleDecl *>(&decl)) {
        Scope *savedScope = currentScope;
        currentScope = &ModuleScopeFor(modDecl->name, *currentScope);
        for (const auto &item : modDecl->items) {
            ApplyDeclImports(*item);
        }
        currentScope = savedScope;
    }
}

Scope &AnalysisContext::ModuleScopeFor(const std::string &name, Scope &parent) {
    return programIndex.ModuleScopeFor(name, parent);
}

std::string AnalysisContext::ModulePathForImport(const UseDecl &d) {
    if (d.path.size() <= 1) {
        return "";
    }
    if (d.kind == UseDecl::Kind::Single) {
        if (d.path.size() <= 2) {
            return "";
        }
        return JoinPathSegments(d.path, 1, d.path.size() - 1);
    }
    return JoinPathSegments(d.path, 1, d.path.size());
}

std::string AnalysisContext::LogicalModulePathForImport(const UseDecl &d) {
    if (d.kind == UseDecl::Kind::Single) {
        if (d.path.size() <= 1) {
            return "";
        }
        return JoinPathSegments(d.path, 0, d.path.size() - 1);
    }
    return JoinPathSegments(d.path, 0, d.path.size());
}

std::string AnalysisContext::ImportScopeDisplayName(const std::string &pkgName, const std::string &modulePath) {
    if (modulePath.empty()) {
        return std::format("package '{}'", pkgName);
    }
    return std::format("module '{}'", modulePath);
}

AnalysisContext::ImportModuleLookup AnalysisContext::FindImportModule(const std::string &pkgName,
                                                                      const std::string &modulePath,
                                                                      const std::string &logicalModulePath) const {
    const std::string packageId = ResolvePackageImport(imports, currentPackage, pkgName);
    if (auto pkgIt = packageModuleScopes.find(packageId); pkgIt != packageModuleScopes.end()) {
        if (auto modIt = pkgIt->second.find(modulePath); modIt != pkgIt->second.end()) {
            return {{&modIt->second->Table(), ImportScopeDisplayName(pkgName, modulePath), packageId, modulePath}, {}};
        }
    }

    std::vector<std::pair<std::string, Scope *>> matches;
    for (const auto &[candidatePackage, moduleScopes] : packageModuleScopes) {
        if (logicalModulePath.empty())
            break;
        if (imports.contains(currentPackage) && candidatePackage != currentPackage && candidatePackage != packageId)
            continue;
        auto modIt = moduleScopes.find(logicalModulePath);
        if (modIt == moduleScopes.end()) {
            continue;
        }
        if (std::ranges::none_of(matches, [&](const auto &match) { return match.second == modIt->second; })) {
            matches.emplace_back(candidatePackage, modIt->second);
        }
    }

    std::ranges::sort(matches, {}, &std::pair<std::string, Scope *>::first);
    ImportModuleLookup lookup;
    if (matches.size() > 1) {
        for (const auto &[candidatePackage, _] : matches)
            lookup.ambiguousPackages.push_back(candidatePackage);
    }
    else if (!matches.empty()) {
        lookup.scope = {&matches[0].second->Table(), ImportScopeDisplayName(matches[0].first, logicalModulePath),
                        matches[0].first, logicalModulePath};
    }
    return lookup;
}

AnalysisContext::ImportScope AnalysisContext::ResolveImportScope(const UseDecl &d, const std::string &pkgName,
                                                                 const std::string &modulePath) {
    const std::string logicalModulePath = LogicalModulePathForImport(d);
    ImportModuleLookup lookup = FindImportModule(pkgName, modulePath, logicalModulePath);
    if (lookup.scope.table) {
        return std::move(lookup.scope);
    }
    if (!lookup.ambiguousPackages.empty()) {
        std::vector<std::string> notes;
        for (const auto &candidatePackage : lookup.ambiguousPackages) {
            notes.push_back(
                std::format("module '{}' is available from package '{}'", logicalModulePath, candidatePackage));
        }
        EmitError(d.location, std::format("module '{}' is ambiguous", logicalModulePath), std::move(notes),
                  std::format("qualify the import with one of the listed package names"));
        return {};
    }
    if (!packageModuleScopes.contains(ResolvePackageImport(imports, currentPackage, pkgName))) {
        EmitError(d.location, std::format("package or module '{}' is not defined", pkgName));
    }
    else {
        EmitError(d.location, std::format("module '{}' was not found in package '{}'", modulePath, pkgName));
    }
    return {};
}

[[nodiscard]] AnalysisContext::ModuleWalk AnalysisContext::WalkModulePath(const std::string &package,
                                                                          const std::string &path) const {
    ModuleWalk walk;
    const auto packageIt = packageModuleScopes.find(package);
    if (path.empty() || packageIt == packageModuleScopes.end()) {
        return walk;
    }
    const auto rootIt = packageIt->second.find("");
    if (rootIt == packageIt->second.end()) {
        return walk;
    }
    const Scope *scope = rootIt->second;
    std::size_t begin = 0;
    while (scope) {
        const std::size_t separator = path.find("::", begin);
        const std::string segment =
            path.substr(begin, separator == std::string::npos ? std::string::npos : separator - begin);
        const auto found = scope->Table().find(segment);
        if (found == scope->Table().end() || found->second.kind != Symbol::Kind::Module) {
            return {};
        }
        if (!walk.inaccessible && package != currentPackage && !IsAccessible(found->second)) {
            walk.inaccessible = &found->second;
        }
        if (separator == std::string::npos) {
            walk.module = &found->second;
            return walk;
        }
        scope = found->second.moduleScope;
        begin = separator + 2;
    }
    return {};
}

[[nodiscard]] const Symbol *AnalysisContext::InaccessibleModule(const std::string &package,
                                                                const std::string &path) const {
    return WalkModulePath(package, path).inaccessible;
}

[[nodiscard]] std::optional<Symbol> AnalysisContext::AccessibleImport(const Symbol &symbol) const {
    if (symbol.kind != Symbol::Kind::Func || symbol.funcOverloads.empty()) {
        return IsAccessible(symbol) ? std::optional<Symbol>(symbol) : std::nullopt;
    }
    Symbol accessible = symbol;
    std::erase_if(accessible.funcOverloads, [this](const FuncDecl *overload) { return !IsAccessible(*overload); });
    if (accessible.funcOverloads.empty()) {
        return std::nullopt;
    }
    accessible.isPublic = true;
    accessible.isEffectivelyPublic = true;
    return accessible;
}

void AnalysisContext::PromoteFromPackage(const UseDecl &d, const std::string &pkgName, const std::string &name) {
    const std::string modulePath = ModulePathForImport(d);
    ImportScope scope = ResolveImportScope(d, pkgName, modulePath);
    if (!scope.table) {
        return;
    }
    if (const Symbol *module = InaccessibleModule(scope.ownerPackage, scope.modulePath)) {
        EmitPrivacyError(d.location, *module);
        return;
    }
    // A primitive is built in, so no package declares it: importing one imports that package's extension of it.
    if (const PrimitiveInfo *primitive = FindPrimitive(name)) {
        ImportPrimitiveExtension(d, scope, *primitive, name);
        return;
    }
    auto sym_it = scope.table->find(name);
    if (sym_it == scope.table->end()) {
        std::string message = std::format("name '{}' was not found in {}", name, scope.displayName);
        std::optional<std::string> help;
        // The item is not at this path, but if one of the package's modules holds it, point at an import that
        // reaches it. A module path that repeats the package name is spelled once, as written in its declaration,
        // when that spelling resolves to the same module.
        if (auto pkgIt = packageModuleScopes.find(scope.ownerPackage); pkgIt != packageModuleScopes.end()) {
            for (const auto &[candidateModule, candidateScope] : pkgIt->second) {
                if (candidateModule.empty() || !candidateScope->Table().contains(name)) {
                    continue;
                }
                std::string spelled = std::format("{}::{}", pkgName, candidateModule);
                if (candidateModule.starts_with(pkgName + "::")) {
                    const std::string relative = candidateModule.substr(pkgName.size() + 2);
                    if (FindImportModule(pkgName, relative, candidateModule).scope.table == &candidateScope->Table())
                        spelled = candidateModule;
                }
                help = std::format("did you mean 'import {}::{}'?", spelled, name);
                break;
            }
        }
        EmitError(d.location, std::move(message), {}, std::move(help));
        return;
    }
    const std::optional<Symbol> accessible = AccessibleImport(sym_it->second);
    if (!accessible) {
        EmitPrivacyError(d.location, sym_it->second);
        return;
    }
    DefineImportedSymbol(*accessible);
    if (accessible->kind == Symbol::Kind::Type && accessible->declaration) {
        explicitTypeImports[currentFile].insert(accessible->declaration);
    }
    ImportSignatureDependencies(*accessible, *scope.table);
}

void AnalysisContext::DefineImportedSymbol(const Symbol &sym) {
    if (Symbol *existing = currentScope->LookupLocal(sym.name)) {
        if (existing->kind == sym.kind && existing->location.line == sym.location.line &&
            existing->location.column == sym.location.column && existing->ownerPackage == sym.ownerPackage &&
            existing->declaration == sym.declaration) {
            *existing = sym;
            return;
        }
    }
    currentScope->Define(sym, diags, currentFile);
}

void AnalysisContext::ImportSignatureDependencies(const Symbol &sym,
                                                  const std::unordered_map<std::string, Symbol> &sourceTable) {
    if (sym.kind != Symbol::Kind::Func) {
        return;
    }

    auto findPackageType = [&](const std::string &name) -> const Symbol * {
        auto sameSymbol = [](const Symbol &lhs, const Symbol &rhs) {
            return lhs.kind == rhs.kind && lhs.name == rhs.name && lhs.location.line == rhs.location.line &&
                   lhs.location.column == rhs.location.column;
        };

        const Symbol *matched = nullptr;
        for (const auto &[_, moduleScopes] : packageModuleScopes) {
            for (const auto &[__, scope] : moduleScopes) {
                const auto &table = scope->Table();
                auto it = table.find(name);
                if (it == table.end()) {
                    continue;
                }
                if (it->second.kind != Symbol::Kind::Type && it->second.kind != Symbol::Kind::Interface) {
                    continue;
                }
                if (matched && !sameSymbol(*matched, it->second)) {
                    return nullptr;
                }
                matched = &it->second;
            }
        }
        return matched;
    };

    auto importNamedType = [&](const std::string &name) {
        if (currentScope->Lookup(name)) {
            return;
        }
        auto depIt = sourceTable.find(name);
        const Symbol *dep = depIt == sourceTable.end() ? findPackageType(name) : &depIt->second;
        if (!dep) {
            return;
        }
        if (dep->kind == Symbol::Kind::Type || dep->kind == Symbol::Kind::Interface) {
            if (const std::optional<Symbol> accessible = AccessibleImport(*dep)) {
                DefineImportedSymbol(*accessible);
            }
        }
    };

    auto visitType = [&](this auto &&self, const TypeExpr &type) -> void {
        if (const auto *named = dynamic_cast<const NamedTypeExpr *>(&type)) {
            importNamedType(named->name);
            for (const auto &arg : named->typeArgs) {
                self(*arg);
            }
        }
        else if (const auto *ptr = dynamic_cast<const PointerTypeExpr *>(&type)) {
            self(*ptr->pointee);
        }
        else if (const auto *reference = dynamic_cast<const ReferenceTypeExpr *>(&type)) {
            self(*reference->pointee);
        }
        else if (const auto *array = dynamic_cast<const ArrayTypeExpr *>(&type)) {
            self(*array->element);
        }
        else if (const auto *slice = dynamic_cast<const SliceTypeExpr *>(&type)) {
            self(*slice->element);
        }
        else if (const auto *range = dynamic_cast<const RangeTypeExpr *>(&type)) {
            if (range->start) {
                self(*range->start);
            }
            if (range->end) {
                self(*range->end);
            }
        }
        else if (const auto *tuple = dynamic_cast<const TupleTypeExpr *>(&type)) {
            for (const auto &elem : tuple->elements) {
                self(*elem);
            }
        }
        else if (const auto *fn = dynamic_cast<const FunctionTypeExpr *>(&type)) {
            for (const auto &param : fn->params) {
                self(*param);
            }
            if (fn->returnType) {
                self(**fn->returnType);
            }
        }
        else {
            for (const TypeExpr *child : NativeTypeChildren(type)) {
                self(*child);
            }
        }
    };

    for (const auto *overload : sym.funcOverloads) {
        for (const auto &param : overload->params) {
            visitType(*param.type);
        }
        if (overload->returnType) {
            visitType(**overload->returnType);
        }
    }
}

void AnalysisContext::CheckUseDecl(const UseDecl &d) {
    if (d.path.empty()) {
        EmitError(d.location, "empty import path");
        return;
    }
    const std::string &pkgName = d.path[0];

    if (d.kind == UseDecl::Kind::Single) {
        // Bind the module at the package-relative `modulePath`, else at a non-empty `logicalModulePath`, as an alias
        // usable through `::`. Returns true when the path names a module, whether or not the importer may name it.
        const auto bindModule = [&](const std::string &modulePath, const std::string &logicalModulePath) -> bool {
            const ImportModuleLookup lookup = FindImportModule(pkgName, modulePath, logicalModulePath);
            if (!lookup.scope.table || lookup.scope.modulePath.empty()) {
                return false;
            }
            const ModuleWalk walk = WalkModulePath(lookup.scope.ownerPackage, lookup.scope.modulePath);
            if (!walk.module) {
                return false;
            }
            if (walk.inaccessible) {
                EmitPrivacyError(d.location, *walk.inaccessible);
                return true;
            }
            DefineImportedSymbol(*walk.module);
            return true;
        };

        // Bare `import Pkg;` binds the package's eponymous module as a
        // namespace, so its members are reached through `Pkg::Name`.
        if (d.path.size() < 2) {
            if (bindModule(pkgName, {})) {
                return;
            }
            EmitError(d.location, std::format("import '{}' does not name a module", pkgName), {},
                      std::format("import an item instead, for example 'import {}::Name'", pkgName));
            return;
        }
        const std::string &name = d.path.back();
        // Item and module imports find modules alike: the package-relative path first, then the logical path, which
        // repeats the package name as `module Pkg::Shapes` does. A module at the package-relative path wins, then an
        // item there, then a module at the logical path.
        const std::string modulePath = JoinPathSegments(d.path, 1, d.path.size());
        if (bindModule(modulePath, {})) {
            return;
        }
        const ImportScope itemScope =
            FindImportModule(pkgName, ModulePathForImport(d), LogicalModulePathForImport(d)).scope;
        if ((!itemScope.table || !itemScope.table->contains(name)) &&
            bindModule(modulePath, JoinPathSegments(d.path, 0, d.path.size()))) {
            return;
        }
        PromoteFromPackage(d, pkgName, name);
    }
    else if (d.kind == UseDecl::Kind::Multi) {
        for (const auto &name : d.names) {
            PromoteFromPackage(d, pkgName, name);
        }
    }
    else // Glob: promote all from the specific module (or all modules
    // if Pkg::*)
    {
        const std::string modulePath = ModulePathForImport(d);
        ImportScope scope = ResolveImportScope(d, pkgName, modulePath);
        if (!scope.table) {
            return;
        }
        if (const Symbol *module = InaccessibleModule(scope.ownerPackage, scope.modulePath)) {
            EmitPrivacyError(d.location, *module);
            return;
        }
        for (const auto &[name, sym] : *scope.table) {
            if (const std::optional<Symbol> accessible = AccessibleImport(sym)) {
                DefineImportedSymbol(*accessible);
                if (accessible->kind == Symbol::Kind::Type && accessible->declaration) {
                    explicitTypeImports[currentFile].insert(accessible->declaration);
                }
            }
        }
        if (const auto *extended = programIndex.PrimitiveExtensions(scope.ownerPackage, scope.modulePath)) {
            for (const TypeRef::Kind kind : *extended) {
                primitiveExtensionImports[currentFile][kind].insert(scope.ownerPackage);
            }
        }
    }
}

std::string AnalysisContext::MangleTypeName(const TypeRef &type) {
    const std::string out = type.MangledSpelling();
    return out.empty() ? "_" : out;
}
} // namespace Rux::SemanticDetail
