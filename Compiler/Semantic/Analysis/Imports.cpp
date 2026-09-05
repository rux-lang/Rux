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

AnalysisContext::ImportScope AnalysisContext::ResolveImportScope(const UseDecl &d, const std::string &pkgName,
                                                                 const std::string &modulePath) {
    const std::string logicalModulePath = LogicalModulePathForImport(d);
    if (auto pkgIt = packageModuleScopes.find(pkgName); pkgIt != packageModuleScopes.end()) {
        if (auto modIt = pkgIt->second.find(modulePath); modIt != pkgIt->second.end()) {
            return {&modIt->second->Table(), ImportScopeDisplayName(pkgName, modulePath), pkgName, modulePath};
        }
    }

    std::vector<std::pair<std::string, Scope *>> matches;
    for (const auto &[candidatePackage, moduleScopes] : packageModuleScopes) {
        auto modIt = moduleScopes.find(logicalModulePath);
        if (modIt == moduleScopes.end()) {
            continue;
        }
        if (std::ranges::none_of(matches, [&](const auto &match) { return match.second == modIt->second; })) {
            matches.emplace_back(candidatePackage, modIt->second);
        }
    }

    std::ranges::sort(matches, {}, &std::pair<std::string, Scope *>::first);
    if (matches.size() > 1) {
        std::vector<std::string> notes;
        for (const auto &[candidatePackage, _] : matches) {
            notes.push_back(
                std::format("module '{}' is available from package '{}'", logicalModulePath, candidatePackage));
        }
        EmitError(d.location, std::format("module '{}' is ambiguous", logicalModulePath), std::move(notes),
                  std::format("qualify the import with one of the listed package names"));
        return {};
    }
    if (!matches.empty()) {
        return {&matches[0].second->Table(), ImportScopeDisplayName(matches[0].first, logicalModulePath),
                matches[0].first, logicalModulePath};
    }

    if (!packageModuleScopes.contains(pkgName)) {
        EmitError(d.location, std::format("package or module '{}' is not defined", pkgName));
    }
    else {
        EmitError(d.location, std::format("module '{}' was not found in package '{}'", modulePath, pkgName));
    }
    return {};
}

[[nodiscard]] const Symbol *AnalysisContext::InaccessibleModule(const std::string &package,
                                                                const std::string &path) const {
    if (package == currentPackage || path.empty()) {
        return nullptr;
    }
    const auto packageIt = packageModuleScopes.find(package);
    if (packageIt == packageModuleScopes.end()) {
        return nullptr;
    }
    const auto rootIt = packageIt->second.find("");
    if (rootIt == packageIt->second.end()) {
        return nullptr;
    }
    const Scope *scope = rootIt->second;
    std::size_t begin = 0;
    while (begin < path.size()) {
        const std::size_t separator = path.find("::", begin);
        const std::string segment =
            path.substr(begin, separator == std::string::npos ? std::string::npos : separator - begin);
        const auto found = scope->Table().find(segment);
        if (found == scope->Table().end() || found->second.kind != Symbol::Kind::Module) {
            return nullptr;
        }
        if (!IsAccessible(found->second)) {
            return &found->second;
        }
        scope = found->second.moduleScope;
        if (!scope || separator == std::string::npos) {
            break;
        }
        begin = separator + 2;
    }
    return nullptr;
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
    auto sym_it = scope.table->find(name);
    if (sym_it == scope.table->end()) {
        std::string message = std::format("name '{}' was not found in {}", name, scope.displayName);
        std::optional<std::string> help;
        // The item is not at this path, but if one of the package's modules
        // holds it, point at the fully-qualified import.
        if (auto pkgIt = packageModuleScopes.find(pkgName); pkgIt != packageModuleScopes.end()) {
            for (const auto &[candidateModule, candidateScope] : pkgIt->second) {
                if (!candidateModule.empty() && candidateScope->Table().contains(name)) {
                    help = std::format("did you mean 'import {}::{}::{}'?", pkgName, candidateModule, name);
                    break;
                }
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
    ImportSignatureDependencies(*accessible, *scope.table);
}

void AnalysisContext::DefineImportedSymbol(const Symbol &sym) {
    if (Symbol *existing = currentScope->LookupLocal(sym.name)) {
        if (existing->kind == sym.kind && existing->location.line == sym.location.line &&
            existing->location.column == sym.location.column) {
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
        else if (const auto *slice = dynamic_cast<const ArrayTypeExpr *>(&type)) {
            self(*slice->element);
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
        // Bind `packageModuleScopes[pkgName][moduleName]` as a module alias
        // usable through `::`. Returns true when the module exists.
        auto bindModuleAlias = [&](const std::string &moduleName) -> bool {
            auto pkgIt = packageModuleScopes.find(pkgName);
            if (pkgIt == packageModuleScopes.end()) {
                return false;
            }
            auto modIt = pkgIt->second.find(moduleName);
            if (modIt == pkgIt->second.end()) {
                return false;
            }
            const auto root = pkgIt->second.find("");
            if (root == pkgIt->second.end()) {
                return false;
            }
            const auto module = root->second->Table().find(moduleName);
            if (module == root->second->Table().end() || module->second.kind != Symbol::Kind::Module) {
                return false;
            }
            if (!IsAccessible(module->second)) {
                EmitPrivacyError(d.location, module->second);
                return true;
            }
            DefineImportedSymbol(module->second);
            return true;
        };

        // Bare `import Pkg;` binds the package's eponymous module as a
        // namespace, so its members are reached through `Pkg::Name`.
        if (d.path.size() < 2) {
            if (bindModuleAlias(pkgName)) {
                return;
            }
            EmitError(d.location, std::format("import '{}' does not name a module", pkgName), {},
                      std::format("import an item instead, for example 'import {}::Name'", pkgName));
            return;
        }
        const std::string &name = d.path.back();
        // If path.size()==2 and name matches a logical module, create a
        // module alias.
        if (d.path.size() == 2 && bindModuleAlias(name)) {
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
            }
        }
    }
}

std::string AnalysisContext::MangleTypeName(const TypeRef &type) {
    std::string out;
    for (const char c : type.ToString()) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            out += c;
        }
        else {
            out += '_';
        }
    }
    return out.empty() ? "_" : out;
}
} // namespace Rux::SemanticDetail
